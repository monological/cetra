#include "minimize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The updates to wait for while minimized, those got, the frames drawn meanwhile, the last
// update's time.
static struct {
    int wanted, updates, drawn;
    double last_update;
} g;

bool minimize_parse(int argc, char** argv, int* i) {
    if (*i + 1 >= argc || strcmp(argv[*i], "--minimize") != 0)
        return false;
    g.wanted = atoi(argv[++*i]);
    return true;
}

static void read_done(Engine* engine, Picture* picture, void* user) {
    (void)user;
    printf("canvas minimized read %s, iconified %d\n", picture ? "a picture" : "NULL",
           engine->iconified);
    fflush(stdout);
    free_picture(picture);
}

void minimize_update(Engine* engine) {
    if (g.wanted <= 0)
        return;
    const double now = glfwGetTime();
    if (!engine->iconified && g.updates == 0) {
        // Frames asked for until the second, so an idle loop reaches it.
        if (engine->total_frames < 2) {
            engine_request_frame(engine);
        } else if (engine->total_frames == 2) {
            printf("canvas minimize after frame %zu\n", engine->total_frames);
            fflush(stdout);
            glfwIconifyWindow(engine->window);
        }
    } else if (engine->iconified) {
        g.updates++;
        printf("canvas minimized update %d after %.3f s, drawn %d\n", g.updates,
               now - g.last_update, g.drawn);
        fflush(stdout);
        if (g.updates == 1)
            engine_read_window(engine, 0.0f, 0.0f, 10.0f, 10.0f, read_done, NULL);
        if (g.updates >= g.wanted)
            glfwRestoreWindow(engine->window);
        else
            engine_request_frame_after(engine, 0.05);
    }
    g.last_update = now;
}

void minimize_drawn(Engine* engine) {
    if (engine->iconified)
        g.drawn++;
    if (g.wanted > 0 && !engine->iconified && g.updates >= g.wanted) {
        printf("canvas restored, drawn while minimized %d\n", g.drawn);
        fflush(stdout);
        g.wanted = 0;
        glfwSetWindowShouldClose(engine->window, GLFW_TRUE);
    }
}
