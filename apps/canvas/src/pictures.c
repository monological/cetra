#include "pictures.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
    // --minimize: the updates it waits for while minimized, those it got, the overlays drawn
    // meanwhile, and the last update's time.
    int minimize, minimized_updates, minimized_draws;
    double last_update;
} g;

// A picture as a binary PPM, its alpha left out.
static void write_ppm(const Picture* picture, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "canvas: cannot write %s\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", picture->width, picture->height);
    const size_t pixels = (size_t)picture->width * (size_t)picture->height;
    for (size_t k = 0; k < pixels; k++)
        fwrite(picture->rgba + k * 4, 1, 3, f);
    fclose(f);
}

bool pictures_parse(int argc, char** argv, int* i) {
    const char* flag = argv[*i];
    const bool takes = strcmp(flag, "--read-window") == 0 || strcmp(flag, "--read-canvas") == 0 ||
                       strcmp(flag, "--read-view3d") == 0 ||
                       strcmp(flag, "--offscreen-view") == 0 || strcmp(flag, "--minimize") == 0;
    if (!takes || *i + 1 >= argc)
        return false;
    char* value = argv[++*i];
    if (strcmp(flag, "--minimize") == 0) {
        g.minimize = atoi(value);
    } else if (strcmp(flag, "--read-window") == 0) {
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
    } else {
        g.offscreen_path = value;
    }
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

// Framebuffer pixels a point across, as the engine takes the window's.
static float window_ratio(const Engine* engine) {
    return engine->win_width > 0 ? (float)engine->fb_width / (float)engine->win_width : 1.0f;
}

void pictures_lay_out(const Engine* engine, const SceneView* view3d) {
    if (!g.offscreen || !view3d)
        return;
    // Its pixels the pane's exactly: the pane's rounded width, given back in points.
    const float s = window_ratio(engine);
    const float w = roundf((view3d->x + view3d->width) * s) - roundf(view3d->x * s);
    const float h = roundf((view3d->y + view3d->height) * s) - roundf(view3d->y * s);
    g.offscreen->width = w / s;
    g.offscreen->height = h / s;
    g.offscreen->camera = view3d->camera;
    g.offscreen->settings = view3d->settings;
}

static void read_window_done(Engine* engine, Picture* picture, void* user) {
    (void)engine;
    (void)user;
    if (picture)
        write_ppm(picture, g.window_path);
    free_picture(picture);
}

// Where a rectangle of points stands in the framebuffer, said once.
static void say_rect(const Engine* engine, const char* what, float x, float y, float w, float h,
                     bool* said) {
    if (*said)
        return;
    const float s = window_ratio(engine);
    const long left = lroundf(x * s), top = lroundf(y * s);
    printf("canvas picture %s at %ld %ld size %ld %ld\n", what, left, top,
           lroundf((x + w) * s) - left, lroundf((y + h) * s) - top);
    fflush(stdout);
    *said = true;
}

static void minimized_read_done(Engine* engine, Picture* picture, void* user) {
    (void)user;
    printf("canvas minimized read %s, iconified %d\n", picture ? "a picture" : "NULL",
           engine->iconified);
    fflush(stdout);
    free_picture(picture);
}

void pictures_update(Engine* engine) {
    if (g.minimize <= 0)
        return;
    const double now = glfwGetTime();
    if (!engine->iconified && g.minimized_updates == 0) {
        // Frames asked for until the second, so an idle loop reaches it.
        if (engine->total_frames < 2) {
            engine_request_frame(engine);
        } else if (engine->total_frames == 2) {
            printf("canvas minimize after frame %zu\n", engine->total_frames);
            fflush(stdout);
            glfwIconifyWindow(engine->window);
        }
    } else if (engine->iconified) {
        g.minimized_updates++;
        printf("canvas minimized update %d after %.3f s, drawn %d\n", g.minimized_updates,
               now - g.last_update, g.minimized_draws);
        fflush(stdout);
        if (g.minimized_updates == 1)
            engine_read_window(engine, 0.0f, 0.0f, 10.0f, 10.0f, minimized_read_done, NULL);
        if (g.minimized_updates >= g.minimize)
            glfwRestoreWindow(engine->window);
        else
            engine_request_frame_after(engine, 0.05);
    }
    g.last_update = now;
}

void pictures_read(Engine* engine, Canvas* canvas, const CanvasView* view,
                   const SceneView* view3d) {
    if (engine->iconified)
        g.minimized_draws++;
    if (g.minimize > 0 && g.minimized_updates > 0 && !engine->iconified &&
        g.minimized_updates >= g.minimize) {
        printf("canvas restored, drawn while minimized %d\n", g.minimized_draws);
        fflush(stdout);
        g.minimize = 0;
        glfwSetWindowShouldClose(engine->window, GLFW_TRUE);
    }
    if (g.window_path) {
        const float* r = g.window_rect;
        say_rect(engine, "window", r[0], r[1], r[2], r[3], &g.said_window);
        engine_read_window(engine, r[0], r[1], r[2], r[3], read_window_done, NULL);
    }
    if (g.canvas_path && canvas && view) {
        Picture* picture = canvas_read_picture(canvas, view, g.canvas_ppp);
        if (picture) {
            say_rect(engine, "canvas", view->x, view->y, view->width, view->height, &g.said_canvas);
            write_ppm(picture, g.canvas_path);
        }
        free_picture(picture);
    }
    if (g.view3d_path && view3d) {
        Picture* picture = scene_view_read_picture(view3d);
        if (picture) {
            say_rect(engine, "view3d", view3d->x, view3d->y, view3d->width, view3d->height,
                     &g.said_view3d);
            write_ppm(picture, g.view3d_path);
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
            write_ppm(picture, g.offscreen_path);
        }
        free_picture(picture);
    }
}
