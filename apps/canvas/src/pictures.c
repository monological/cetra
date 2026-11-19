#include "pictures.h"

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

bool pictures_parse(int argc, char** argv, int* i) {
    if (*i + 1 >= argc)
        return false;
    const char* flag = argv[*i];
    char* value = argv[*i + 1];
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
    // The pane's rectangle, rounded at its place as the pane is, so its pixels are the pane's.
    g.offscreen->x = view3d->x;
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
    if (g.minimize > 0 && !engine->iconified && g.minimized_updates >= g.minimize) {
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
