/*
 * The 2D canvas's own app (spec 14.0): test sheets drawn through canvas.h, in a
 * window to pan and zoom, or headless for a picture.
 *
 * A sheet is laid out in millimetres and drawn in nanometres, the units pcb
 * gives the canvas, so every length here goes through MM().
 *
 * Under --canvas-probe a sheet also prints where each of its shapes landed and
 * how big it is, in framebuffer pixels with rows from the top, so the `canvas`
 * gate group can measure the picture against closed forms without restating
 * the sheet's layout.
 *
 * A debug harness for two internal headers: the glyphs sheet measures text
 * against stb_truetype's own rasterizer, which only its header reaches, and
 * --edit-thread fills an edit through the engine's thread shim, which is what
 * builds on Windows as well.
 *
 * Under --idle the engine draws only when something asks (spec 14.1), and a
 * headless run prints a line a frame and one at exit, which the `idle` gate
 * group reads. --gui-click hands the debug GUI a click through cimgui, since a
 * scripted pointer reaches the engine's input and not ImGui's.
 */

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "cetra/camera.h"
#include "cetra/canvas.h"
#include "cetra/engine.h"
#include "cetra/fuzzy.h"
#include "cetra/glyph_cache.h"
#include "cetra/internal/ext/stb_truetype.h"
#include "cetra/internal/text_doc_edit.h" // text_doc_ranges, the parse's errors as they stand
#include "cetra/internal/thread.h"
#include "cetra/internal/ui_editor_hooks.h" // where the editor drew a range, for the probe
#include "cetra/postfx.h"
#include "cetra/program.h"
#include "cetra/scene.h"
#include "cetra/text.h"
#include "cetra/text_doc.h"
#include "cetra/triangulate.h"
#include "cetra/ui.h"
#include "cetra/ui_editor.h"
#include "cetra/ui_editor_vim.h"
#include "cetra/util.h"

// Dear ImGui via cimgui, for --gui-click's click handed to the debug GUI.
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"

#include "canvas_fonts.h" // the fonts built into the program, by cetra_embed_files

#define MM(v) ((int64_t)llround((v) * 1e6))

// --dash, --cap and --join: the stroke every paint a sheet makes starts with, its fills' and its
// text's among them, which the canvas leaves as they are. What a sheet sets on a paint stands.
static CanvasDash g_dash;
static int g_cap = -1, g_join = -1; // -1: the canvas's own default

static CanvasPaint paint_of(CanvasLayer* layer, uint16_t style, uint32_t net, uint32_t object) {
    CanvasPaint paint = {layer, style, net, object};
    if (g_dash.length)
        paint.dash = g_dash;
    if (g_cap >= 0)
        paint.cap = (CanvasCap)g_cap;
    if (g_join >= 0)
        paint.join = (CanvasJoin)g_join;
    return paint;
}

// A paint with butt ends.
static CanvasPaint butt(CanvasPaint paint) {
    paint.cap = CANVAS_CAP_BUTT;
    return paint;
}

static Canvas* g_canvas;
static CanvasView g_views[2]; // the window's views, side by side under --split
static int g_view_count = 1;
static int g_only_view = -1;     // under --only-view, the one of the split drawn
static int g_drag_view = -1;     // the view a drag pans
static float g_last_x, g_last_y; // the cursor, window points
static bool g_probe_target;      // say once which framebuffer the overlay drew into
static bool g_budget;            // time each frame's canvas draws

// What a sheet is built with, and what it says about a split's two views.
typedef struct SheetContext {
    Canvas* canvas;
    CanvasEdit* edit;
    int64_t offset;         // added to every coordinate
    const CanvasView* view; // the view it will be drawn in
    double fb_scale;        // framebuffer pixels per window point
    bool probe;             // print where its shapes land
    bool edit_thread;       // fill the edit on a thread of its own, where a sheet can
    Engine* engine;         // under idle mode that thread fills the edit while the loop runs
    const Font* font;       // what its text is set in

    // Written by a sheet that has two sides: the layers a split's mirrored
    // view shows (empty = every one), and each view's active layer.
    CanvasLayerSet under_shown;
    const CanvasLayer* active[2];
} SheetContext;

// Framebuffer pixels per unit in the sheet's view.
static double ppu_px(const SheetContext* ctx) {
    return ctx->view->points_per_unit * ctx->fb_scale;
}

// A unit's place in the framebuffer, rows from the top.
static void fb_of(const SheetContext* ctx, CanvasPoint p, double* x, double* y) {
    float px, py;
    canvas_view_point_of(ctx->view, p, &px, &py);
    *x = px * ctx->fb_scale;
    *y = py * ctx->fb_scale;
}

// Prints a cell's box, `half` units each way of its centre, then the caller's
// measures; the line is ended by the caller.
static void probe_cell(const SheetContext* ctx, const char* sheet, const char* name,
                       CanvasPoint centre, int64_t half) {
    double x0, y0, x1, y1;
    fb_of(ctx, (CanvasPoint){centre.x - half, centre.y + half}, &x0, &y0);
    fb_of(ctx, (CanvasPoint){centre.x + half, centre.y - half}, &x1, &y1);
    printf("canvas %s %s box %ld %ld %ld %ld", sheet, name, lround(x0), lround(y0), lround(x1),
           lround(y1));
}

// Prints the framebuffer pixel a point lands in and the colour it must be.
static void probe_pixel(const SheetContext* ctx, const char* sheet, const char* name, CanvasPoint p,
                        uint32_t rgb) {
    if (!ctx->probe)
        return;
    double x, y;
    fb_of(ctx, p, &x, &y);
    printf("canvas %s %s at %ld %ld want %06x\n", sheet, name, lround(x), lround(y), rgb);
}

/*
 * The shapes sheet: one row of each kind, at sizes from a hair to a pad.
 */
static void sheet_shapes(SheetContext* ctx) {
    Canvas* canvas = ctx->canvas;
    CanvasEdit* edit = ctx->edit;
    const int64_t offset = ctx->offset;
    CanvasLayer* copper = canvas_add_layer(canvas, "copper");
    CanvasLayer* silk = canvas_add_layer(canvas, "silk");
    const uint16_t orange = canvas_add_style(canvas, 0xc8873affu);
    const uint16_t white = canvas_add_style(canvas, 0xe6e6e6ffu);
    const uint16_t blue = canvas_add_style(canvas, 0x4f8fd8ffu);
    const uint16_t green = canvas_add_style(canvas, 0x58b368ffu);
    const CanvasPaint cu = paint_of(copper, orange, 0, 0);
    const CanvasPaint sk = paint_of(silk, white, 0, 0);
#define AT(x, y) ((CanvasPoint){MM(x) + offset, MM(y) + offset})

    // Segments: widths from 0.02 mm to 1 mm, round ends, then flat.
    const double widths[] = {0.02, 0.05, 0.1, 0.25, 0.5, 1.0};
    for (int i = 0; i < 6; i++) {
        const double x = -10.0 + i * 2.2;
        canvas_edit_segment(
            edit, &(CanvasSegment){
                      .paint = cu, .a = AT(x, 5.0), .b = AT(x + 1.4, 7.0), .width = MM(widths[i])});
        canvas_edit_segment(edit, &(CanvasSegment){.paint = butt(cu),
                                                   .a = AT(x + 0.8, 5.0),
                                                   .b = AT(x + 1.6, 5.6),
                                                   .width = MM(widths[i])});
    }

    // Arcs: a quarter, a major arc, a clockwise one, flat ends, a half turn.
    const CanvasPaint bl = paint_of(copper, blue, 0, 0);
    canvas_edit_arc(edit, &(CanvasArc){.paint = bl,
                                       .a = AT(-9.0, 1.5),
                                       .b = AT(-10.0, 2.5),
                                       .radius = MM(1.0),
                                       .width = MM(0.25)});
    canvas_edit_arc(edit, &(CanvasArc){.paint = bl,
                                       .a = AT(-6.5, 1.5),
                                       .b = AT(-7.5, 2.5),
                                       .radius = MM(1.0),
                                       .width = MM(0.25),
                                       .major = true});
    canvas_edit_arc(edit, &(CanvasArc){.paint = bl,
                                       .a = AT(-3.5, 1.5),
                                       .b = AT(-4.5, 2.5),
                                       .radius = MM(1.0),
                                       .width = MM(0.25),
                                       .clockwise = true});
    canvas_edit_arc(edit, &(CanvasArc){.paint = butt(bl),
                                       .a = AT(-1.0, 1.5),
                                       .b = AT(-2.0, 2.5),
                                       .radius = MM(1.0),
                                       .width = MM(0.4)});
    canvas_edit_arc(edit, &(CanvasArc){.paint = bl,
                                       .a = AT(1.0, 2.0),
                                       .b = AT(3.0, 2.0),
                                       .radius = MM(1.0),
                                       .width = MM(0.1)});
    canvas_edit_arc(edit, &(CanvasArc){.paint = bl,
                                       .a = AT(4.0, 2.0),
                                       .b = AT(6.0, 2.0),
                                       .radius = MM(1.0),
                                       .width = MM(0.1),
                                       .clockwise = true});

    // Circles: filled, rings, and one a hair across.
    const CanvasPaint gr = paint_of(copper, green, 0, 0);
    canvas_edit_circle(edit,
                       &(CanvasCircle){.paint = gr, .centre = AT(-9.5, -1.0), .radius = MM(0.5)});
    canvas_edit_circle(edit,
                       &(CanvasCircle){.paint = gr, .centre = AT(-7.0, -1.0), .radius = MM(1.0)});
    canvas_edit_circle(
        edit, &(CanvasCircle){
                  .paint = gr, .centre = AT(-4.0, -1.0), .radius = MM(1.0), .stroke = MM(0.1)});
    canvas_edit_circle(
        edit, &(CanvasCircle){
                  .paint = gr, .centre = AT(-1.0, -1.0), .radius = MM(1.0), .stroke = MM(0.4)});
    canvas_edit_circle(edit,
                       &(CanvasCircle){.paint = gr, .centre = AT(1.0, -1.0), .radius = MM(0.02)});

    // Boxes: a pad, a turned one, an outline, a stadium, a thin one at 45 degrees.
    canvas_edit_box(edit, &(CanvasBox){.paint = cu,
                                       .centre = AT(-9.5, -4.5),
                                       .size_x = MM(1.0),
                                       .size_y = MM(0.6),
                                       .corner = MM(0.15)});
    canvas_edit_box(edit, &(CanvasBox){.paint = cu,
                                       .centre = AT(-7.0, -4.5),
                                       .size_x = MM(1.6),
                                       .size_y = MM(0.8),
                                       .corner = MM(0.1),
                                       .angle = 0.5235987755982988});
    canvas_edit_box(edit, &(CanvasBox){.paint = cu,
                                       .centre = AT(-4.0, -4.5),
                                       .size_x = MM(2.0),
                                       .size_y = MM(1.2),
                                       .corner = MM(0.3),
                                       .stroke = MM(0.1)});
    canvas_edit_box(edit, &(CanvasBox){.paint = cu,
                                       .centre = AT(-1.0, -4.5),
                                       .size_x = MM(2.0),
                                       .size_y = MM(0.8),
                                       .corner = MM(0.4)});
    canvas_edit_box(edit, &(CanvasBox){.paint = cu,
                                       .centre = AT(2.0, -4.5),
                                       .size_x = MM(2.4),
                                       .size_y = MM(0.3),
                                       .angle = 0.7853981633974483});

    // A resistor's zigzag and a closed square, in silk over the copper.
    const CanvasPoint zig[] = {AT(4.0, -1.0), AT(4.5, -0.4), AT(5.0, -1.6), AT(5.5, -0.4),
                               AT(6.0, -1.6), AT(6.5, -0.4), AT(7.0, -1.0)};
    canvas_edit_polyline(
        edit, &(CanvasPolyline){.paint = sk, .points = zig, .count = 7, .width = MM(0.15)});
    const CanvasPoint square[] = {AT(4.5, -5.5), AT(6.5, -5.5), AT(6.5, -3.5), AT(4.5, -3.5)};
    canvas_edit_polyline(
        edit, &(CanvasPolyline){
                  .paint = sk, .points = square, .count = 4, .width = MM(0.1), .closed = true});
    // Silk across a copper pad, to show the order layers draw in.
    canvas_edit_segment(
        edit, &(CanvasSegment){
                  .paint = sk, .a = AT(-10.5, -4.5), .b = AT(-8.5, -4.5), .width = MM(0.12)});
#undef AT
}

// White on a black background, so a pixel's red code over 255 is its coverage.
static CanvasPaint white_paint(Canvas* canvas) {
    return paint_of(canvas_add_layer(canvas, "white"), canvas_add_style(canvas, 0xffffffffu), 0, 0);
}

/*
 * The areas sheet: one shape a cell, each with an area in closed form.
 */
static void sheet_areas(SheetContext* ctx) {
    CanvasEdit* edit = ctx->edit;
    const CanvasPaint paint = white_paint(ctx->canvas);
    const double s = ppu_px(ctx);
    const int64_t half = MM(0.75);
#define CELL(i, j) \
    ((CanvasPoint){MM(-3.2 + 1.6 * (i)) + ctx->offset, MM(1.5 - 3.0 * (j)) + ctx->offset})
#define PROBE(name, centre, fmt, ...)                     \
    do {                                                  \
        if (ctx->probe) {                                 \
            probe_cell(ctx, "areas", name, centre, half); \
            printf(" " fmt "\n", __VA_ARGS__);            \
        }                                                 \
    } while (0)

    CanvasPoint c = CELL(0, 0);
    canvas_edit_circle(edit, &(CanvasCircle){.paint = paint, .centre = c, .radius = MM(0.5)});
    PROBE("circle", c, "r %.6f", MM(0.5) * s);

    c = CELL(1, 0);
    canvas_edit_circle(
        edit, &(CanvasCircle){.paint = paint, .centre = c, .radius = MM(0.5), .stroke = MM(0.1)});
    PROBE("ring", c, "r %.6f s %.6f", MM(0.5) * s, MM(0.1) * s);

    c = CELL(2, 0);
    canvas_edit_box(
        edit,
        &(CanvasBox){
            .paint = paint, .centre = c, .size_x = MM(1.2), .size_y = MM(0.8), .corner = MM(0.2)});
    PROBE("rbox", c, "w %.6f h %.6f c %.6f", MM(1.2) * s, MM(0.8) * s, MM(0.2) * s);

    // A capsule turned 30 degrees: its ends rounded onto units, its length measured from them.
    c = CELL(3, 0);
    const CanvasPoint ca = {c.x - MM(0.5 * 0.8660254037844387), c.y - MM(0.5 * 0.5)};
    const CanvasPoint cb = {c.x + MM(0.5 * 0.8660254037844387), c.y + MM(0.5 * 0.5)};
    canvas_edit_segment(edit, &(CanvasSegment){.paint = paint, .a = ca, .b = cb, .width = MM(0.2)});
    PROBE("capsule", c, "len %.6f w %.6f", hypot((double)(cb.x - ca.x), (double)(cb.y - ca.y)) * s,
          MM(0.2) * s);

    // A quarter of a circle about the cell's centre, counter-clockwise from east to north.
    c = CELL(0, 1);
    canvas_edit_arc(edit, &(CanvasArc){.paint = paint,
                                       .a = {c.x + MM(0.5), c.y},
                                       .b = {c.x, c.y + MM(0.5)},
                                       .radius = MM(0.5),
                                       .width = MM(0.1)});
    PROBE("arc", c, "r %.6f w %.6f sweep %.9f", MM(0.5) * s, MM(0.1) * s, 1.5707963267948966);

    c = CELL(1, 1);
    canvas_edit_box(edit, &(CanvasBox){.paint = paint,
                                       .centre = c,
                                       .size_x = MM(1.0),
                                       .size_y = MM(0.3),
                                       .angle = 0.7853981633974483});
    PROBE("turned", c, "w %.6f h %.6f", MM(1.0) * s, MM(0.3) * s);

    c = CELL(2, 1);
    canvas_edit_box(
        edit,
        &(CanvasBox){
            .paint = paint, .centre = c, .size_x = MM(1.0), .size_y = MM(0.6), .stroke = MM(0.1)});
    PROBE("outline", c, "w %.6f h %.6f s %.6f", MM(1.0) * s, MM(0.6) * s, MM(0.1) * s);

    // A flat-ended bar turned -20 degrees.
    c = CELL(3, 1);
    const CanvasPoint ba = {c.x - MM(0.5 * 0.9396926207859084), c.y + MM(0.5 * 0.3420201433256687)};
    const CanvasPoint bb = {c.x + MM(0.5 * 0.9396926207859084), c.y - MM(0.5 * 0.3420201433256687)};
    canvas_edit_segment(edit,
                        &(CanvasSegment){.paint = butt(paint), .a = ba, .b = bb, .width = MM(0.2)});
    PROBE("bar", c, "len %.6f w %.6f", hypot((double)(bb.x - ba.x), (double)(bb.y - ba.y)) * s,
          MM(0.2) * s);

    // Three quarters of a circle with flat ends, clockwise from south round to east: an arc of
    // more than half a turn, whose flat ends' inner ramp is measured only near each end.
    c = CELL(4, 0);
    canvas_edit_arc(edit, &(CanvasArc){.paint = butt(paint),
                                       .a = {c.x, c.y - MM(0.5)},
                                       .b = {c.x + MM(0.5), c.y},
                                       .radius = MM(0.5),
                                       .width = MM(0.1),
                                       .clockwise = true,
                                       .major = true});
    PROBE("flat-arc", c, "r %.6f w %.6f sweep %.9f", MM(0.5) * s, MM(0.1) * s, 4.71238898038469);
#undef PROBE
#undef CELL
}

/*
 * The polygons sheet: filled polygons, concave and holed, one a cell. Each
 * prints its area by the shoelace formula over the units it was given, its
 * outline's length, and the area of its own triangulation, which must agree.
 */
typedef struct Poly {
    CanvasPoint pts[160];
    size_t n;
    size_t holes[8];
    size_t hole_count;
} Poly;

// A point at (x, y) millimetres from the centre, turned by `turn` radians.
static void poly_add(Poly* p, CanvasPoint centre, double x, double y, double turn) {
    const double c = cos(turn), s = sin(turn);
    p->pts[p->n++] = (CanvasPoint){centre.x + MM(x * c - y * s), centre.y + MM(x * s + y * c)};
}

// A regular ring of `sides` points `radius` millimetres about (x, y) from the centre.
static void poly_ring(Poly* p, CanvasPoint centre, double x, double y, double radius, int sides,
                      double turn) {
    for (int i = 0; i < sides; i++) {
        const double a = turn + 6.283185307179586 * i / sides;
        poly_add(p, centre, x + radius * cos(a), y + radius * sin(a), 0.0);
    }
}

static void poly_hole(Poly* p) {
    p->holes[p->hole_count++] = p->n;
}

static void probe_polygon(const SheetContext* ctx, const char* name, CanvasPoint centre,
                          const Poly* p) {
    const double s = ppu_px(ctx);
    double* xy = malloc(p->n * 2 * sizeof(double));
    if (!xy)
        return;
    for (size_t i = 0; i < p->n; i++) {
        xy[2 * i] = (double)(p->pts[i].x - p->pts[0].x);
        xy[2 * i + 1] = (double)(p->pts[i].y - p->pts[0].y);
    }
    double area = 0.0, perimeter = 0.0;
    for (size_t r = 0; r <= p->hole_count; r++) {
        const size_t start = r ? p->holes[r - 1] : 0;
        const size_t end = r < p->hole_count ? p->holes[r] : p->n;
        double ring = 0.0;
        for (size_t i = start, j = end - 1; i < end; j = i++) {
            const double* a = xy + 2 * j;
            const double* b = xy + 2 * i;
            ring += a[0] * b[1] - b[0] * a[1];
            perimeter += hypot(b[0] - a[0], b[1] - a[1]);
        }
        area += r == 0 ? fabs(ring) / 2.0 : -fabs(ring) / 2.0;
    }
    Triangulation t;
    double tri_area = 0.0;
    if (triangulate_polygon(xy, p->n, p->holes, p->hole_count, &t)) {
        for (size_t k = 0; k < t.count; k++) {
            const double* a = xy + 2 * t.indices[3 * k];
            const double* b = xy + 2 * t.indices[3 * k + 1];
            const double* c = xy + 2 * t.indices[3 * k + 2];
            tri_area += fabs((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])) / 2.0;
        }
    }
    probe_cell(ctx, "polygons", name, centre, MM(1.2));
    printf(" area %.6f perimeter %.6f triangulated %.6f triangles %zu points %zu holes %zu\n",
           area * s * s, perimeter * s, tri_area * s * s, t.count, p->n, p->hole_count);
    free_triangulation(&t);
    free(xy);
}

static void sheet_polygons(SheetContext* ctx) {
    const CanvasPaint paint = white_paint(ctx->canvas);
#define CELL(i, j) \
    ((CanvasPoint){MM(-2.6 + 2.6 * (i)) + ctx->offset, MM(1.5 - 3.0 * (j)) + ctx->offset})
    static Poly p;
    const struct {
        const char* name;
        int i, j;
    } cells[6] = {{"holed", 0, 0},  {"star", 1, 0},  {"ell", 2, 0},
                  {"holes3", 0, 1}, {"round", 1, 1}, {"comb", 2, 1}};
    for (int k = 0; k < 6; k++) {
        const CanvasPoint c = CELL(cells[k].i, cells[k].j);
        memset(&p, 0, sizeof(p));
        switch (k) {
            case 0: // a square with a square hole, its edges off the pixel grid
                poly_add(&p, c, -1.003, -1.003, 0.0), poly_add(&p, c, 1.003, -1.003, 0.0);
                poly_add(&p, c, 1.003, 1.003, 0.0), poly_add(&p, c, -1.003, 1.003, 0.0);
                poly_hole(&p);
                poly_add(&p, c, -0.497, -0.497, 0.0), poly_add(&p, c, 0.497, -0.497, 0.0);
                poly_add(&p, c, 0.497, 0.497, 0.0), poly_add(&p, c, -0.497, 0.497, 0.0);
                break;
            case 1: // a five-pointed star
                for (int s = 0; s < 10; s++) {
                    const double r = s % 2 ? 0.4 : 1.0;
                    const double a = 1.5707963267948966 + 6.283185307179586 * s / 10.0;
                    poly_add(&p, c, r * cos(a), r * sin(a), 0.0);
                }
                break;
            case 2: // an L turned 15 degrees
                poly_add(&p, c, -0.8, -0.8, 0.2618), poly_add(&p, c, 0.8, -0.8, 0.2618);
                poly_add(&p, c, 0.8, -0.3, 0.2618), poly_add(&p, c, -0.3, -0.3, 0.2618);
                poly_add(&p, c, -0.3, 0.8, 0.2618), poly_add(&p, c, -0.8, 0.8, 0.2618);
                break;
            case 3: // a plate with three octagonal holes, the holes wound either way
                poly_add(&p, c, -1.0, -0.8, 0.0), poly_add(&p, c, 1.0, -0.8, 0.0);
                poly_add(&p, c, 1.0, 0.8, 0.0), poly_add(&p, c, -1.0, 0.8, 0.0);
                poly_hole(&p);
                poly_ring(&p, c, -0.55, 0.3, 0.25, 8, 0.1);
                poly_hole(&p);
                poly_ring(&p, c, 0.55, 0.3, 0.25, 8, 0.2);
                poly_hole(&p);
                for (int s = 7; s >= 0; s--) // clockwise
                    poly_add(&p, c, 0.25 * cos(0.3 + 0.7853981633974483 * s),
                             -0.35 + 0.25 * sin(0.3 + 0.7853981633974483 * s), 0.0);
                break;
            case 4: // a circle of 64 sides
                poly_ring(&p, c, 0.0, 0.0, 1.0, 64, 0.05);
                break;
            default: // a comb: six teeth 0.2 wide and 0.16 apart, every gap's corners reflex
                poly_add(&p, c, -1.0, -0.9, 0.0), poly_add(&p, c, 1.0, -0.9, 0.0);
                for (int t = 0; t < 6; t++) {
                    const double right = 1.0 - 0.36 * t, left = right - 0.2;
                    poly_add(&p, c, right, 0.9, 0.0);
                    poly_add(&p, c, left, 0.9, 0.0);
                    if (t < 5) {
                        poly_add(&p, c, left, -0.4, 0.0);
                        poly_add(&p, c, left - 0.16, -0.4, 0.0);
                    }
                }
                break;
        }
        canvas_edit_polygon(ctx->edit, &(CanvasPolygon){.paint = paint,
                                                        .points = p.pts,
                                                        .count = p.n,
                                                        .holes = p.holes,
                                                        .hole_count = p.hole_count});
        if (ctx->probe)
            probe_polygon(ctx, cells[k].name, c, &p);
    }
#undef CELL
}

/*
 * The ramp sheet: boxes whose left edge falls a known fraction across a pixel.
 * The pixel the edge crosses holds 1 - f, the one before it 0 and the one
 * after 1: coverage is clamp(0.5 - d) with d the distance past the edge.
 */
static void sheet_ramp(SheetContext* ctx) {
    const CanvasPaint paint = white_paint(ctx->canvas);
    const double s = ppu_px(ctx);
    const double fb_w = ctx->view->width * ctx->fb_scale;
    const double fb_h = ctx->view->height * ctx->fb_scale;
    const double fractions[] = {0.1, 0.3, 0.5, 0.7, 0.9};
    for (int i = 0; i < 5; i++) {
        // The edge at pixel column k plus f, from the view's centre; with this
        // view a pixel is a whole number of units, so the edge lands exactly.
        const int k = (int)(fb_w * 0.15) + i * (int)(fb_w * 0.16);
        const int64_t edge = llround((k + fractions[i] - fb_w * 0.5) / s);
        const int64_t top = llround((fb_h * 0.25) / s), bottom = -top;
        canvas_edit_box(ctx->edit,
                        &(CanvasBox){.paint = paint,
                                     .centre = {edge + MM(0.25) + ctx->offset, ctx->offset},
                                     .size_x = MM(0.5),
                                     .size_y = top - bottom});
        if (ctx->probe)
            printf("canvas ramp f%.1f col %d rows %ld %ld want %.6f\n", fractions[i], k,
                   lround(fb_h * 0.5 - fb_h * 0.2), lround(fb_h * 0.5 + fb_h * 0.2),
                   1.0 - fractions[i]);
    }
}

/*
 * The thin sheet: lines a tenth of a pixel wide, one across and one down, each
 * off the pixel grid. Widened to a pixel, each column across the first (and
 * each row across the second) sums to exactly one pixel of coverage.
 */
static void sheet_thin(SheetContext* ctx) {
    const CanvasPaint paint = white_paint(ctx->canvas);
    const double s = ppu_px(ctx);
    const int64_t o = ctx->offset;
    const int64_t width = llround(0.1 / s);
    const int64_t across = llround(0.37 / s), down = llround(-0.63 / s);
    canvas_edit_segment(ctx->edit, &(CanvasSegment){.paint = paint,
                                                    .a = {o - MM(3.0), o + across},
                                                    .b = {o + MM(3.0), o + across},
                                                    .width = width});
    canvas_edit_segment(ctx->edit, &(CanvasSegment){.paint = paint,
                                                    .a = {o + down, o - MM(2.0)},
                                                    .b = {o + down, o + MM(2.0)},
                                                    .width = width});
    if (ctx->probe) {
        // Away from the ends and from where the two cross.
        double x0, y0, x1, y1;
        fb_of(ctx, (CanvasPoint){o + MM(1.0), o + across}, &x0, &y0);
        fb_of(ctx, (CanvasPoint){o + MM(2.5), o + across}, &x1, &y1);
        printf("canvas thin across cols %ld %ld rows %ld %ld\n", lround(x0), lround(x1),
               lround(y0) - 4, lround(y0) + 4);
        fb_of(ctx, (CanvasPoint){o + down, o + MM(1.5)}, &x0, &y0);
        fb_of(ctx, (CanvasPoint){o + down, o + MM(0.5)}, &x1, &y1);
        printf("canvas thin down rows %ld %ld cols %ld %ld\n", lround(y0), lround(y1),
               lround(x0) - 4, lround(x0) + 4);
    }
}

/*
 * One line at a slope of 1 in 7, through the origin and through the view's
 * centre at (7, 1) * 2^18: long, reaching about 1.9e9 units each way from the
 * origin and so cut into hundreds of pieces, or short, reaching just past the
 * view either side of its centre. Seen deep in the middle of one of the long
 * line's pieces the two should draw the same pixels.
 */
static void line_sheet(SheetContext* ctx, int64_t centre, int64_t k) {
    const CanvasPaint paint = white_paint(ctx->canvas);
    const int64_t x = ctx->offset + 7 * centre, y = ctx->offset + centre;
    canvas_edit_segment(ctx->edit, &(CanvasSegment){.paint = paint,
                                                    .a = {x - 7 * k, y - k},
                                                    .b = {x + 7 * k, y + k},
                                                    .width = MM(0.002)});
}

static void sheet_long(SheetContext* ctx) {
    line_sheet(ctx, 0, (int64_t)1 << 28);
}

static void sheet_long_short(SheetContext* ctx) {
    line_sheet(ctx, (int64_t)1 << 18, (int64_t)1 << 14);
}

/*
 * The symbol sheet: an integrated circuit as a schematic draws one, its pin
 * names inside the body, the top row's turned to read down it, its pin numbers
 * along the pins, and a row of sizes down to where text stops being drawn.
 */
static void sheet_symbol(SheetContext* ctx) {
    Canvas* canvas = ctx->canvas;
    CanvasEdit* edit = ctx->edit;
    const int64_t offset = ctx->offset;
    CanvasLayer* body = canvas_add_layer(canvas, "body");
    CanvasLayer* graphics = canvas_add_layer(canvas, "graphics");
    CanvasLayer* text = canvas_add_layer(canvas, "text");
    const CanvasPaint fill = paint_of(body, canvas_add_style(canvas, 0xffffc2ffu), 0, 0);
    const CanvasPaint line = paint_of(graphics, canvas_add_style(canvas, 0x840000ffu), 0, 0);
    const CanvasPaint name = paint_of(text, canvas_add_style(canvas, 0x008484ffu), 0, 0);
    const CanvasPaint number = paint_of(text, canvas_add_style(canvas, 0xa90000ffu), 0, 0);
    const CanvasPaint label = paint_of(text, canvas_add_style(canvas, 0x404040ffu), 0, 0);
#define AT(x, y) ((CanvasPoint){MM(x) + offset, MM(y) + offset})
    const double PI = 3.141592653589793, PITCH = 2.54, LEAD = 2.54;
    const double hx = 7.62, hy = 11.43; // the body's half size

    canvas_edit_box(edit, &(CanvasBox){.paint = fill,
                                       .centre = AT(0.0, 0.0),
                                       .size_x = MM(2.0 * hx),
                                       .size_y = MM(2.0 * hy)});
    canvas_edit_box(edit, &(CanvasBox){.paint = line,
                                       .centre = AT(0.0, 0.0),
                                       .size_x = MM(2.0 * hx),
                                       .size_y = MM(2.0 * hy),
                                       .stroke = MM(0.254)});

    static const char* LEFT[] = {"VDD", "PA0", "PA1", "PA2", "NRST", "GND"};
    static const char* RIGHT[] = {"PB0", "PB1/SCL", "PB2/SDA", "PB3", "SWDIO", "SWCLK"};
    static const char* TOP[] = {"VBAT", "VDDA", "VREF+"};
    char digits[8];
    int pin = 1;
    for (int i = 0; i < 6; i++, pin++) {
        const double y = 3.81 - i * PITCH;
        canvas_edit_segment(
            edit, &(CanvasSegment){
                      .paint = line, .a = AT(-hx - LEAD, y), .b = AT(-hx, y), .width = MM(0.152)});
        canvas_edit_text(edit, &(CanvasText){.paint = name,
                                             .font = ctx->font,
                                             .string = LEFT[i],
                                             .anchor = AT(-hx + 0.6, y),
                                             .size = MM(1.27),
                                             .align_y = CANVAS_ALIGN_MIDDLE});
        snprintf(digits, sizeof(digits), "%d", pin);
        canvas_edit_text(edit, &(CanvasText){.paint = number,
                                             .font = ctx->font,
                                             .string = digits,
                                             .anchor = AT(-hx - LEAD * 0.5, y + 0.25),
                                             .size = MM(1.0),
                                             .align_x = CANVAS_ALIGN_CENTRE});
    }
    for (int i = 0; i < 6; i++, pin++) {
        const double y = -8.89 + i * PITCH;
        canvas_edit_segment(
            edit, &(CanvasSegment){
                      .paint = line, .a = AT(hx, y), .b = AT(hx + LEAD, y), .width = MM(0.152)});
        canvas_edit_text(edit, &(CanvasText){.paint = name,
                                             .font = ctx->font,
                                             .string = RIGHT[i],
                                             .anchor = AT(hx - 0.6, y),
                                             .size = MM(1.27),
                                             .align_x = CANVAS_ALIGN_RIGHT,
                                             .align_y = CANVAS_ALIGN_MIDDLE});
        snprintf(digits, sizeof(digits), "%d", pin);
        canvas_edit_text(edit, &(CanvasText){.paint = number,
                                             .font = ctx->font,
                                             .string = digits,
                                             .anchor = AT(hx + LEAD * 0.5, y + 0.25),
                                             .size = MM(1.0),
                                             .align_x = CANVAS_ALIGN_CENTRE});
    }
    // The top row: names turned a quarter clockwise to read down into the body,
    // numbers a quarter the other way along the pin's left side.
    for (int i = 0; i < 3; i++, pin++) {
        const double x = -PITCH + i * PITCH;
        canvas_edit_segment(
            edit, &(CanvasSegment){
                      .paint = line, .a = AT(x, hy), .b = AT(x, hy + LEAD), .width = MM(0.152)});
        canvas_edit_text(edit, &(CanvasText){.paint = name,
                                             .font = ctx->font,
                                             .string = TOP[i],
                                             .anchor = AT(x, hy - 0.6),
                                             .size = MM(1.27),
                                             .angle = -PI * 0.5,
                                             .align_y = CANVAS_ALIGN_MIDDLE});
        snprintf(digits, sizeof(digits), "%d", pin);
        canvas_edit_text(edit, &(CanvasText){.paint = number,
                                             .font = ctx->font,
                                             .string = digits,
                                             .anchor = AT(x - 0.25, hy + LEAD * 0.5),
                                             .size = MM(1.0),
                                             .angle = PI * 0.5,
                                             .align_x = CANVAS_ALIGN_CENTRE});
    }
    canvas_edit_text(edit, &(CanvasText){.paint = label,
                                         .font = ctx->font,
                                         .string = "U1",
                                         .anchor = AT(-hx, hy + 0.6),
                                         .size = MM(1.27),
                                         .align_y = CANVAS_ALIGN_BOTTOM});
    canvas_edit_text(edit, &(CanvasText){.paint = label,
                                         .font = ctx->font,
                                         .string = "STM32G431K",
                                         .anchor = AT(0.0, -hy - 0.6),
                                         .size = MM(1.27),
                                         .align_x = CANVAS_ALIGN_CENTRE,
                                         .align_y = CANVAS_ALIGN_TOP});

    // Beside it: the same words mirrored, as a board's underside reads, and a
    // row of sizes halving down past the canvas's smallest.
    canvas_edit_text(edit, &(CanvasText){.paint = label,
                                         .font = ctx->font,
                                         .string = "Bottom silk",
                                         .anchor = AT(14.0, 9.5),
                                         .size = MM(1.5)});
    canvas_edit_text(edit, &(CanvasText){.paint = label,
                                         .font = ctx->font,
                                         .string = "Bottom silk",
                                         .anchor = AT(14.0, 7.0),
                                         .size = MM(1.5),
                                         .align_x = CANVAS_ALIGN_RIGHT,
                                         .mirrored = true});
    double y = 4.0;
    for (double size = 2.0; size > 0.02; size *= 0.5) {
        char words[32];
        snprintf(words, sizeof(words), "%.3g mm text", size);
        canvas_edit_text(edit, &(CanvasText){.paint = label,
                                             .font = ctx->font,
                                             .string = words,
                                             .anchor = AT(14.0, y),
                                             .size = MM(size)});
        y -= size * 1.2 + 0.3;
    }
#undef AT
}

/*
 * The glyphs sheet: one character a cell, white on black, each the size of a
 * large heading, so the gate can sum its coverage against stb_truetype's own
 * rasterizer at the same em; and one kerned string, whose width it checks
 * against the font's advances and kerning summed here.
 */
static const char GLYPHS[] = "Bg@We";

// A font's atlas read back from the GPU; NULL out of memory.
static unsigned char* atlas_of(const Font* font) {
    unsigned char* atlas = malloc((size_t)font->atlas_width * (size_t)font->atlas_height);
    if (!atlas)
        return NULL;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, font->atlas_texture_id);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, atlas);
    return atlas;
}

// Every glyph's texels in the font's atlas against stb's own distance field for it, rasterised here
// with the parameters the font states, and the count of glyphs whose texels differ.
static void probe_atlas(const Font* font) {
    const stbtt_fontinfo* info = (const stbtt_fontinfo*)font->stb_font_info;
    const int side = font->atlas_width;
    if (font->atlas_height != side)
        return;
    unsigned char* atlas = atlas_of(font);
    if (!atlas)
        return;
    const float scale = stbtt_ScaleForPixelHeight(info, font->base_size);
    const int on_edge =
        (int)lroundf(font->sdf_edge * 255.0f + 0.5f); // the edge is half a code under
    const float per_pixel = (float)on_edge / font->sdf_spread;
    int glyphs = 0, differ = 0;
    for (const GlyphInfo* g = font->glyph_cache; g; g = g->hh.next) {
        int w = 0, h = 0, x0 = 0, y0 = 0;
        unsigned char* sdf =
            stbtt_GetCodepointSDF(info, scale, (int)g->codepoint, (int)font->sdf_spread, on_edge,
                                  per_pixel, &w, &h, &x0, &y0);
        const int u = (int)lroundf(g->u0 * (float)side), v = (int)lroundf(g->v0 * (float)side);
        bool same = w == (int)lroundf((g->u1 - g->u0) * (float)side) && (float)x0 == g->x0 &&
                    (float)y0 == g->y0;
        for (int row = 0; same && row < h; row++)
            same = memcmp(atlas + (size_t)(v + row) * (size_t)side + (size_t)u,
                          sdf + (size_t)row * (size_t)w, (size_t)w) == 0;
        glyphs++;
        differ += !same;
        if (sdf)
            stbtt_FreeSDF(sdf, NULL);
    }
    free(atlas);
    printf("canvas glyphs atlas %d differ %d\n", glyphs, differ);
}

static void sheet_glyphs(SheetContext* ctx) {
    const CanvasPaint paint = white_paint(ctx->canvas);
    const stbtt_fontinfo* info = (const stbtt_fontinfo*)ctx->font->stb_font_info;
    const int64_t size = MM(1.0), half = MM(0.75);
    const double em_px = (double)size * ppu_px(ctx);
    for (int i = 0; GLYPHS[i]; i++) {
        const CanvasPoint centre = {MM(-3.0 + 1.5 * i) + ctx->offset, MM(1.0) + ctx->offset};
        const char one[2] = {GLYPHS[i], 0};
        canvas_edit_text(ctx->edit, &(CanvasText){.paint = paint,
                                                  .font = ctx->font,
                                                  .string = one,
                                                  .anchor = centre,
                                                  .size = size,
                                                  .align_x = CANVAS_ALIGN_CENTRE,
                                                  .align_y = CANVAS_ALIGN_MIDDLE});
        if (!ctx->probe)
            continue;
        // stb's own coverage of the glyph at the same em: an exact-area
        // rasterizer, quantised to a byte a pixel.
        const float scale = stbtt_ScaleForMappingEmToPixels(info, (float)em_px);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(info, GLYPHS[i], scale, scale, &x0, &y0, &x1, &y1);
        const int w = x1 - x0, h = y1 - y0;
        unsigned char* pixels = calloc((size_t)w * (size_t)h, 1);
        double area = 0.0;
        if (pixels) {
            stbtt_MakeCodepointBitmap(info, pixels, w, h, w, scale, scale, GLYPHS[i]);
            for (int k = 0; k < w * h; k++)
                area += pixels[k] / 255.0;
            free(pixels);
        }
        char cell[8];
        snprintf(cell, sizeof(cell), "%c", GLYPHS[i]);
        probe_cell(ctx, "glyphs", cell, centre, half);
        printf(" em %.4f stb %.4f\n", em_px, area);
    }
    const CanvasText kerned = {.paint = paint,
                               .font = ctx->font,
                               .string = "AVATAR To Wave",
                               .anchor = {MM(-3.9) + ctx->offset, MM(-1.5) + ctx->offset},
                               .size = size};
    canvas_edit_text(ctx->edit, &kerned);
    if (ctx->probe) {
        // The font's advances and pair adjustments in its own units, then
        // scaled to the em, all in double.
        const double per_unit = (double)size * stbtt_ScaleForMappingEmToPixels(info, 1.0f);
        double units = 0.0;
        int prev = 0;
        for (const char* p = kerned.string; *p; p++) {
            int advance, bearing;
            stbtt_GetCodepointHMetrics(info, *p, &advance, &bearing);
            units += advance + (prev ? stbtt_GetCodepointKernAdvance(info, prev, *p) : 0);
            prev = *p;
        }
        printf("canvas glyphs width %lld stb %.1f\n", (long long)canvas_text_width(&kerned),
               units * per_unit);
        // Every pair of the atlas's glyphs, the font's kerning asked by codepoint and stepped by
        // its pen, against stb's own at its base size; the pen's second step joined and advancing
        // by the second glyph's width.
        const float base = stbtt_ScaleForPixelHeight(info, ctx->font->base_size);
        int pairs = 0, kerned_pairs = 0, differ = 0, pen_differ = 0;
        for (const GlyphInfo* a = ctx->font->glyph_cache; a; a = a->hh.next)
            for (const GlyphInfo* b = ctx->font->glyph_cache; b; b = b->hh.next) {
                const int kern =
                    stbtt_GetCodepointKernAdvance(info, (int)a->codepoint, (int)b->codepoint);
                pairs++;
                kerned_pairs += kern != 0;
                differ +=
                    font_kern_advance(ctx->font, a->codepoint, b->codepoint) != (float)kern * base;
                FontPen pen = {0};
                font_pen_step(&pen, ctx->font, a->codepoint);
                const FontStep step = font_pen_step(&pen, ctx->font, b->codepoint);
                pen_differ += !step.joined || step.glyph != b || step.kern != (float)kern * base ||
                              step.advance != b->advance_x;
            }
        printf("canvas glyphs pairs %d kerned %d differ %d pen %d\n", pairs, kerned_pairs, differ,
               pen_differ);
        probe_atlas(ctx->font);
    }
}

/*
 * The overlap sheet: two capsules crossing in a layer drawn at half opacity.
 * Where they overlap the layer is drawn once, so the crossing is no brighter
 * than either arm.
 */
static void sheet_overlap(SheetContext* ctx) {
    CanvasPaint paint = white_paint(ctx->canvas);
    paint.layer->opacity = 0.5f;
    const int64_t o = ctx->offset;
    canvas_edit_segment(ctx->edit, &(CanvasSegment){.paint = paint,
                                                    .a = {MM(-2.0) + o, MM(-1.0) + o},
                                                    .b = {MM(2.0) + o, MM(1.0) + o},
                                                    .width = MM(0.8)});
    canvas_edit_segment(ctx->edit, &(CanvasSegment){.paint = paint,
                                                    .a = {MM(-2.0) + o, MM(1.0) + o},
                                                    .b = {MM(2.0) + o, MM(-1.0) + o},
                                                    .width = MM(0.8)});
    if (ctx->probe) {
        double x, y;
        fb_of(ctx, (CanvasPoint){o, o}, &x, &y);
        printf("canvas overlap both at %ld %ld\n", lround(x), lround(y));
        fb_of(ctx, (CanvasPoint){MM(-1.6) + o, MM(-0.8) + o}, &x, &y);
        printf("canvas overlap one at %ld %ld\n", lround(x), lround(y));
    }
}

/*
 * The tags sheet: a box a cell, each drawn through a different tag state, and
 * where each should land on screen. The colour a pixel takes where shapes of a
 * layer overlap is a raised one's, then the first added's.
 */
static void sheet_tags(SheetContext* ctx) {
    Canvas* canvas = ctx->canvas;
    CanvasEdit* edit = ctx->edit;
    CanvasTags* tags = canvas_tags(canvas);
    CanvasLayer* layer = canvas_add_layer(canvas, "tags");
    const uint16_t red = canvas_add_style(canvas, 0xff0000ffu);
    const uint16_t white = canvas_add_style(canvas, 0xffffffffu);
    const int64_t o = ctx->offset;
#define CELL(i) ((CanvasPoint){MM(-3.5 + 1.75 * ((i) % 5)) + o, MM(1.0 - 2.0 * ((i) / 5)) + o})
#define BOX(i, style_, net_, object_)                                                   \
    canvas_edit_box(edit, &(CanvasBox){.paint = paint_of(layer, style_, net_, object_), \
                                       .centre = CELL(i),                               \
                                       .size_x = MM(1.0),                               \
                                       .size_y = MM(1.0)})
    BOX(0, red, 0, 0);
    canvas_tags_set(tags, 1, (CanvasTagState){.colour = 0x00ff00ffu});
    BOX(1, red, 1, 0);
    canvas_tags_set(tags, 2, (CanvasTagState){.colour = 0x0000ffffu});
    BOX(2, red, 1, 2);
    canvas_tags_set(tags, 3, (CanvasTagState){.dim = 0.5f});
    BOX(3, white, 0, 3);
    canvas_tags_set(tags, 4, (CanvasTagState){.hidden = true});
    BOX(4, red, 0, 4);
    // Two boxes overlapping: the second raised, so its colour wins where they
    // meet; then the same pair with neither raised, where the first does. The
    // raised box's left edge lies 0.7 of the way across a pixel, so that pixel
    // is under half covered by it: a fringe, which the box beneath takes.
    const CanvasPoint c5 = CELL(5), c6 = CELL(6);
    double edge_x, edge_y;
    fb_of(ctx, c5, &edge_x, &edge_y);
    const int64_t nudge = llround((floor(edge_x) + 0.7 - edge_x) / ppu_px(ctx));
    canvas_tags_set(tags, 5, (CanvasTagState){.colour = 0x0000ffffu, .raised = true});
    BOX(5, red, 0, 0);
    canvas_edit_box(edit, &(CanvasBox){.paint = paint_of(layer, red, 0, 5),
                                       .centre = {c5.x + MM(0.5) + nudge, c5.y},
                                       .size_x = MM(1.0),
                                       .size_y = MM(1.0)});
    canvas_tags_set(tags, 6, (CanvasTagState){.colour = 0x0000ffffu});
    BOX(6, red, 0, 0);
    canvas_edit_box(edit, &(CanvasBox){.paint = paint_of(layer, red, 0, 6),
                                       .centre = {c6.x + MM(0.5), c6.y},
                                       .size_x = MM(1.0),
                                       .size_y = MM(1.0)});
    // An outline: a ring a little outside the box.
    canvas_tags_set(tags, 7, (CanvasTagState){.outline = 0xffff00ffu});
    BOX(7, red, 0, 7);
    // An outlined trace of two segments, round-joined: the ring stays outside
    // the trace, and at the joint each segment's ring is under the other's body.
    const CanvasPoint c8 = CELL(8);
    canvas_tags_set(tags, 8, (CanvasTagState){.outline = 0xffff00ffu});
    const CanvasPoint trace[] = {
        {c8.x - MM(0.6), c8.y - MM(0.4)}, {c8.x, c8.y}, {c8.x + MM(0.6), c8.y - MM(0.4)}};
    canvas_edit_polyline(edit, &(CanvasPolyline){.paint = paint_of(layer, red, 0, 8),
                                                 .points = trace,
                                                 .count = 3,
                                                 .width = MM(0.3)});
#undef BOX
    if (ctx->probe) {
        // A point well inside each box, and the outline's ring: a box's edge is
        // 0.5 mm from its centre, and its ring 1.75 points past that.
        const double ring_mm = 1.75 / (ctx->view->points_per_unit * 1e6);
        probe_pixel(ctx, "tags", "style", CELL(0), 0xff0000);
        probe_pixel(ctx, "tags", "net-colour", CELL(1), 0x00ff00);
        probe_pixel(ctx, "tags", "object-over-net", CELL(2), 0x0000ff);
        probe_pixel(ctx, "tags", "dim", CELL(3), 0x808080);
        probe_pixel(ctx, "tags", "hidden", CELL(4), 0x000000);
        probe_pixel(ctx, "tags", "raised", (CanvasPoint){c5.x + MM(0.25), c5.y}, 0x0000ff);
        printf("canvas tags raised-fringe at %ld %ld want %06x\n", (long)floor(edge_x),
               (long)floor(edge_y), 0xff0000u);
        probe_pixel(ctx, "tags", "first-added", (CanvasPoint){c6.x + MM(0.25), c6.y}, 0xff0000);
        probe_pixel(ctx, "tags", "outline-body", CELL(7), 0xff0000);
        probe_pixel(ctx, "tags", "outline-ring",
                    (CanvasPoint){CELL(7).x + MM(0.5 + ring_mm), CELL(7).y}, 0xffff00);
        // On the second segment's centre line, as far from the joint as the
        // first segment's ring is from its end: that ring passes through here,
        // inside the second segment's body.
        const double along = 0.15 + ring_mm, unit = sqrt(0.6 * 0.6 + 0.4 * 0.4);
        probe_pixel(ctx, "tags", "outline-joint",
                    (CanvasPoint){c8.x + MM(along * 0.6 / unit), c8.y - MM(along * 0.4 / unit)},
                    0xff0000);
    }
#undef CELL
}

// --dim STRENGTH,ROOM: how far the view fades its tagged shapes not lit, toward white, and the
// points round a lit one it eases off over (spec 14.24).
static float g_dim, g_dim_room;

/*
 * The dim sheet (spec 14.24): four red boxes in a row of a layer drawn under --dim -- one lit, one
 * a hair beside it tagged but not lit, one tagged far from it, and one untagged -- and where each
 * should land on screen: the lit and the untagged red, the far one faded toward white by the
 * strength. The near one is said with no colour: the far one's with no room, nearer red with it.
 */
static void sheet_dim(SheetContext* ctx) {
    Canvas* canvas = ctx->canvas;
    CanvasTags* tags = canvas_tags(canvas);
    CanvasLayer* layer = canvas_add_layer(canvas, "dim");
    const uint16_t red = canvas_add_style(canvas, 0xff0000ffu);
    const int64_t o = ctx->offset;
    canvas_tags_set(tags, 1, (CanvasTagState){.lit = true});
    canvas_tags_set(tags, 2, (CanvasTagState){.colour = 0xff0000ffu});
    // The lit box's right edge at -2.5 mm, the near one's left 0.1 mm past it.
    const CanvasPoint lit = {MM(-3.0) + o, o}, near = {MM(-1.9) + o, o}, far = {MM(3.0) + o, o},
                      untagged = {o, MM(-2.0) + o};
    const struct {
        CanvasPoint at;
        uint32_t object;
    } boxes[] = {{lit, 1}, {near, 2}, {far, 2}, {untagged, 0}};
    for (size_t k = 0; k < sizeof boxes / sizeof boxes[0]; k++)
        canvas_edit_box(ctx->edit, &(CanvasBox){.paint = paint_of(layer, red, 0, boxes[k].object),
                                                .centre = boxes[k].at,
                                                .size_x = MM(1.0),
                                                .size_y = MM(1.0)});
    if (ctx->probe) {
        const uint32_t faded = (uint32_t)lroundf(255.0f * g_dim);
        probe_pixel(ctx, "dim", "lit", lit, 0xff0000);
        probe_pixel(ctx, "dim", "far", far, 0xff0000u | faded << 8 | faded);
        probe_pixel(ctx, "dim", "untagged", untagged, 0xff0000);
        double x, y;
        fb_of(ctx, (CanvasPoint){near.x - MM(0.45), near.y}, &x, &y);
        printf("canvas dim near at %ld %ld\n", lround(x), lround(y));
    }
}

/*
 * The board sheet: a dense two-layer board, made the way pcb would make one.
 * Its layers, styles and tags are set on the main thread, and its shapes are
 * filled into the edit by board_fill, which touches nothing else and so runs
 * on a thread of its own under --edit-thread.
 *
 * Under --canvas-probe it also runs a script, a step a frame after the first
 * draw, printing what each frame uploaded: a tag written, a style written, a
 * segment added, the same removed, and a transient layer refilled twice.
 */
typedef struct Board {
    CanvasLayer *plane, *bottom, *top, *silk, *edge, *transient;
    uint16_t plane_s, bottom_s, top_s, silk_s, edge_s, wire_s;
    const Font* font;
    int64_t offset;
} Board;

static Board g_board;
static void (*g_after_draw)(void); // a sheet's frame script, run after each draw

enum { BOARD_GND = 99, BOARD_HIGHLIGHT = 7, BOARD_PICKED = 701, BOARD_REMOVED = 330 };

static CanvasPoint board_at(const Board* b, double x, double y) {
    return (CanvasPoint){MM(x) + b->offset, MM(y) + b->offset};
}

// A point of the left side's frame turned to side `side`, a quarter turn
// counter-clockwise each.
static void board_turn(int side, double x, double y, double* ox, double* oy) {
    for (int s = 0; s < side; s++) {
        const double t = x;
        x = -y, y = t;
    }
    *ox = x, *oy = y;
}

static void board_pad(CanvasEdit* edit, const Board* b, CanvasLayer* layer, uint16_t style,
                      uint32_t net, uint32_t object, double x, double y, double sx, double sy,
                      double angle) {
    canvas_edit_box(edit, &(CanvasBox){.paint = paint_of(layer, style, net, object),
                                       .centre = board_at(b, x, y),
                                       .size_x = MM(sx),
                                       .size_y = MM(sy),
                                       .corner = MM(fmin(sx, sy) * 0.25),
                                       .angle = angle});
}

// A plated hole: a ring on each copper layer from the drill's radius out to the
// pad's, so the hole shows whatever is under it.
static void board_via(CanvasEdit* edit, const Board* b, uint32_t net, uint32_t object, double x,
                      double y, double ring, double hole) {
    const CanvasCircle pad = {.paint = paint_of(b->top, b->top_s, net, object),
                              .centre = board_at(b, x, y),
                              .radius = MM((ring + hole) * 0.5),
                              .stroke = MM(ring - hole)};
    canvas_edit_circle(edit, &pad);
    CanvasCircle under = pad;
    under.paint.layer = b->bottom, under.paint.style = b->bottom_s;
    canvas_edit_circle(edit, &under);
}

// A trace 0.2 mm wide through points given in the left side's frame, turned to
// `side`.
static void board_route(CanvasEdit* edit, const Board* b, CanvasPaint paint, int side,
                        const double (*points)[2], size_t count) {
    CanvasPoint line[4];
    for (size_t i = 0; i < count && i < 4; i++) {
        double x, y;
        board_turn(side, points[i][0], points[i][1], &x, &y);
        line[i] = board_at(b, x, y);
    }
    canvas_edit_polyline(edit, &(CanvasPolyline){.paint = paint,
                                                 .points = line,
                                                 .count = count < 4 ? count : 4,
                                                 .width = MM(0.2)});
}

static void board_label(CanvasEdit* edit, const Board* b, const char* text, double x, double y,
                        double size) {
    canvas_edit_text(edit, &(CanvasText){.paint = paint_of(b->silk, b->silk_s, 0, 0),
                                         .font = b->font,
                                         .string = text,
                                         .anchor = board_at(b, x, y),
                                         .size = MM(size),
                                         .align_x = CANVAS_ALIGN_CENTRE,
                                         .align_y = CANVAS_ALIGN_MIDDLE});
}

static void board_fill(CanvasEdit* edit, const Board* b) {
    const double QUARTER = 1.5707963267948966;
    // The outline: a 60 by 40 mm board, its corners rounded.
    CanvasPoint outline[4 * 9];
    size_t n = 0;
    for (int c = 0; c < 4; c++) {
        const double cx = (c == 0 || c == 3) ? 27.0 : -27.0, cy = c < 2 ? 17.0 : -17.0;
        for (int k = 0; k <= 8; k++) {
            const double t = (c + k / 8.0) * QUARTER;
            outline[n++] = board_at(b, cx + 3.0 * cos(t), cy + 3.0 * sin(t));
        }
    }
    canvas_edit_polyline(edit, &(CanvasPolyline){.paint = paint_of(b->edge, b->edge_s, 0, 0),
                                                 .points = outline,
                                                 .count = n,
                                                 .width = MM(0.15),
                                                 .closed = true});

    // The ground plane, cleared round every via and mounting hole.
    enum { RING = 12, VIAS = 48, MOUNTS = 4 };
    static CanvasPoint plane[4 + (VIAS + MOUNTS) * RING];
    static size_t holes[VIAS + MOUNTS];
    size_t pn = 0, hn = 0;
    plane[pn++] = board_at(b, -29.0, -19.0), plane[pn++] = board_at(b, 29.0, -19.0);
    plane[pn++] = board_at(b, 29.0, 19.0), plane[pn++] = board_at(b, -29.0, 19.0);

    // U1, a 48-pin quad flat pack: each side's twelve pads fan out on the top
    // layer to a via, and the bottom layer carries them on -- to U2 or U3 from
    // the middle of the left and right sides, to the edge fingers from the
    // bottom side, and to test points from the rest.
    for (int side = 0; side < 4; side++) {
        for (int k = 0; k < 12; k++) {
            const uint32_t pin = (uint32_t)(side * 12 + k + 1);
            const double y = (5.5 - k) * 0.5, vy = 2.2 * y;
            double x0, y0;
            board_turn(side, -4.35, y, &x0, &y0);
            board_pad(edit, b, b->top, b->top_s, pin, 200 + pin, x0, y0, 1.4, 0.3, side * QUARTER);
            const double route[4][2] = {
                {-4.35, y}, {-6.0, y}, {-6.0 - 1.2 * fabs(y), vy}, {-10.5, vy}};
            board_route(edit, b, paint_of(b->top, b->top_s, pin, 300 + pin), side, route, 4);
            double vx, vyy;
            board_turn(side, -10.5, vy, &vx, &vyy);
            board_via(edit, b, pin, 400 + pin, vx, vyy, 0.3, 0.15);
            holes[hn++] = pn;
            for (int i = 0; i < RING; i++) {
                const double t = i * 4.0 * QUARTER / RING;
                plane[pn++] = board_at(b, vx + 0.55 * cos(t), vyy - 0.55 * sin(t));
            }

            double bottom[4][2];
            size_t bn = 0;
            if ((side == 0 || side == 2) && k >= 4 && k <= 7) {
                const double py = (1.5 - (k - 4)) * 1.27;
                const double r[4][2] = {
                    {-10.5, vy}, {-13.0, vy}, {-13.0 - fabs(vy - py), py}, {-17.3, py}};
                memcpy(bottom, r, sizeof(r)), bn = 4;
            } else if (side == 1) {
                const double r[4][2] = {{-10.5, vy},
                                        {-12.0, vy},
                                        {-12.0 - 0.8 * fabs(vy), 1.8 * vy},
                                        {-17.5, 1.8 * vy}};
                memcpy(bottom, r, sizeof(r)), bn = 4;
                double fx, fy;
                board_turn(side, -18.5, 1.8 * vy, &fx, &fy);
                board_pad(edit, b, b->bottom, b->bottom_s, pin, 600 + pin, fx, fy, 2.5, 1.2,
                          side * QUARTER);
            } else {
                const double r[2][2] = {{-10.5, vy}, {-14.5, vy}};
                memcpy(bottom, r, sizeof(r)), bn = 2;
                double tx, ty;
                board_turn(side, -15.0, vy, &tx, &ty);
                canvas_edit_circle(
                    edit, &(CanvasCircle){.paint = paint_of(b->bottom, b->bottom_s, pin, 600 + pin),
                                          .centre = board_at(b, tx, ty),
                                          .radius = MM(0.45)});
            }
            board_route(edit, b, paint_of(b->bottom, b->bottom_s, pin, 500 + pin), side, bottom,
                        bn);
        }
    }
    canvas_edit_box(edit, &(CanvasBox){.paint = paint_of(b->silk, b->silk_s, 0, 0),
                                       .centre = board_at(b, 0.0, 0.0),
                                       .size_x = MM(7.0),
                                       .size_y = MM(7.0),
                                       .stroke = MM(0.15)});
    canvas_edit_circle(edit, &(CanvasCircle){.paint = paint_of(b->silk, b->silk_s, 0, 0),
                                             .centre = board_at(b, -2.8, 2.8),
                                             .radius = MM(0.3)});
    board_label(edit, b, "U1", 0.0, 0.0, 1.5);

    // U2 and U3, eight-pin small outlines whose inner rows the bottom layer
    // reaches through vias of their own.
    for (int u = 0; u < 2; u++) {
        const double cx = u == 0 ? -20.0 : 20.0, dir = u == 0 ? 1.0 : -1.0;
        for (int row = 0; row < 2; row++)
            for (int k = 0; k < 4; k++) {
                const double py = (1.5 - k) * 1.27, px = cx + (row ? -2.7 : 2.7) * dir;
                board_pad(edit, b, b->top, b->top_s, 0, 0, px, py, 1.5, 0.6, 0.0);
                if (row == 0)
                    board_via(edit, b, 0, 0, px, py, 0.3, 0.15);
            }
        canvas_edit_box(edit, &(CanvasBox){.paint = paint_of(b->silk, b->silk_s, 0, 0),
                                           .centre = board_at(b, cx, 0.0),
                                           .size_x = MM(3.9),
                                           .size_y = MM(5.4),
                                           .stroke = MM(0.15)});
        board_label(edit, b, u == 0 ? "U2" : "U3", cx, 3.9, 1.0);
    }

    // Decoupling capacitors at U1's corners, turned 45 degrees, and a column of
    // resistors at each end.
    static const char* CAPS[] = {"C1", "C2", "C3", "C4"};
    for (int c = 0; c < 4; c++) {
        const double sx = (c == 0 || c == 3) ? 1.0 : -1.0, sy = c < 2 ? 1.0 : -1.0;
        const double cx = 7.2 * sx, cy = 7.2 * sy, angle = QUARTER * 0.5 * sx * sy;
        for (int p = 0; p < 2; p++) {
            const double d = p ? 0.75 : -0.75;
            board_pad(edit, b, b->top, b->top_s, p ? BOARD_GND : 0, 700 + 2 * c + p + 1,
                      cx + d * cos(angle), cy + d * sin(angle), 0.8, 0.95, angle);
        }
        board_label(edit, b, CAPS[c], cx + 1.9 * sx, cy + 0.6 * sy, 0.9);
    }
    for (int r = 0; r < 8; r++) {
        const double cx = r < 4 ? -26.0 : 26.0, cy = -6.0 + (r % 4) * 4.0;
        char name[8];
        snprintf(name, sizeof(name), "R%d", r + 1);
        board_pad(edit, b, b->top, b->top_s, 0, 0, cx - 0.8, cy, 0.8, 0.95, 0.0);
        board_pad(edit, b, b->top, b->top_s, 0, 0, cx + 0.8, cy, 0.8, 0.95, 0.0);
        board_label(edit, b, name, cx, cy + 1.3, 0.9);
    }

    // Mounting holes, plated, on the ground net.
    for (int m = 0; m < MOUNTS; m++) {
        const double mx = (m & 1) ? 25.5 : -25.5, my = (m & 2) ? 15.5 : -15.5;
        board_via(edit, b, BOARD_GND, 0, mx, my, 1.6, 1.1);
        holes[hn++] = pn;
        for (int i = 0; i < RING; i++) {
            const double t = i * 4.0 * QUARTER / RING;
            plane[pn++] = board_at(b, mx + 2.0 * cos(t), my - 2.0 * sin(t));
        }
    }
    canvas_edit_polygon(edit,
                        &(CanvasPolygon){.paint = paint_of(b->plane, b->plane_s, BOARD_GND, 0),
                                         .points = plane,
                                         .count = pn,
                                         .holes = holes,
                                         .hole_count = hn});
    board_label(edit, b, "CETRA CANVAS  REV A", 0.0, 17.6, 1.2);
    board_label(edit, b, "J1", 0.0, -14.6, 1.0);
}

static void* board_thread(void* edit) {
    board_fill((CanvasEdit*)edit, &g_board);
    return NULL;
}

// The board's shapes in, then one trace queued for removal: its pad and its via stay.
static void board_finish(Canvas* canvas, CanvasEdit* edit) {
    canvas_apply(canvas, edit);
    canvas_edit_remove_object(edit, BOARD_REMOVED);
}

/*
 * Under --idle with --edit-thread the board arrives as pcb's would: filled on a
 * thread while the loop runs, after a pause standing for a parse so that it
 * ends while the loop waits. The update hook starts it late (start_late_work)
 * and applies it on the frame its end wakes. --work-delay MS sets the pause.
 */
static unsigned g_work_delay_ms = 50;
static CanvasEdit* g_late_edit;  // the board's edit, until it is applied
static atomic_bool g_late_ready; // the thread has filled it
static bool g_late_thread_live;  // a thread was started and not yet joined
static cetra_thread_t g_late_thread;

static void* board_late_thread(void* engine) {
    cetra_sleep_ms(g_work_delay_ms);
    board_fill(g_late_edit, &g_board);
    atomic_store(&g_late_ready, true);
    engine_end_work(engine);
    return NULL;
}

// Begun as the thread starts and not before: headless, frames wait for work out (spec 14.7), so
// work begun frames ahead of its start would hold the run waiting for a frame that starts it.
static void board_late_start(Engine* engine) {
    engine_begin_work(engine);
    g_late_thread_live = cetra_thread_create(&g_late_thread, board_late_thread, engine);
    if (!g_late_thread_live)
        board_late_thread(engine);
}

// Applies the board once its thread has filled it, or at exit only joins the thread, which may
// still be filling it, and drops it: a thread outliving the engine would end its work on freed
// memory. True when it applied the board.
static bool board_late_settle(bool at_exit) {
    if (!g_late_edit || (!at_exit && !atomic_load(&g_late_ready)))
        return false;
    if (g_late_thread_live)
        cetra_thread_join(g_late_thread);
    g_late_thread_live = false;
    if (!at_exit) {
        board_finish(g_canvas, g_late_edit);
        canvas_apply(g_canvas, g_late_edit);
    }
    free_canvas_edit(g_late_edit);
    g_late_edit = NULL;
    return !at_exit;
}

static void board_script(void) {
    static int frame;
    static uint64_t uploads, bytes, generation;
    const uint64_t du = g_canvas->uploads - uploads, db = g_canvas->bytes_uploaded - bytes;
    const uint64_t dg = canvas_generation(g_canvas) - generation;
    static const char* STEPS[] = {"first",  "tag",       "style",          "add",
                                  "remove", "transient", "transient-again"};
    if (frame < (int)(sizeof(STEPS) / sizeof(STEPS[0])))
        printf("canvas board %s uploads %llu bytes %llu generation %llu\n", STEPS[frame],
               (unsigned long long)du, (unsigned long long)db, (unsigned long long)dg);
    uploads = g_canvas->uploads, bytes = g_canvas->bytes_uploaded;
    generation = canvas_generation(g_canvas);

    // The next frame's step.
    const Board* b = &g_board;
    CanvasEdit* edit = create_canvas_edit(g_canvas);
    switch (frame) {
        case 0:
            canvas_tags_set(canvas_tags(g_canvas), 702, (CanvasTagState){.outline = 0xffffffffu});
            break;
        case 1:
            canvas_set_style(g_canvas, b->silk_s, 0xf0f0d0ffu);
            break;
        case 2:
            canvas_edit_segment(edit, &(CanvasSegment){.paint = paint_of(b->top, b->top_s, 0, 9001),
                                                       .a = board_at(b, 1.0, 1.0),
                                                       .b = board_at(b, 1.5, 1.5),
                                                       .width = MM(0.2)});
            break;
        case 3:
            canvas_edit_remove_object(edit, 9001);
            break;
        case 4:
        case 5:
            // A transient layer, cleared and filled again: fifty lines in one
            // tile, as a rubber band or a ratsnest would be each frame.
            canvas_edit_clear_layer(edit, b->transient);
            for (int i = 0; i < 50; i++)
                canvas_edit_segment(
                    edit, &(CanvasSegment){.paint = paint_of(b->transient, b->wire_s, 0, 0),
                                           .a = board_at(b, 0.5, 0.5 + i * 0.05),
                                           .b = board_at(b, 3.5, 0.5 + i * 0.06 + frame * 0.1),
                                           .width = MM(0.02)});
            break;
        default:
            break;
    }
    canvas_apply(g_canvas, edit);
    free_canvas_edit(edit);
    frame++;
    fflush(stdout);
}

static void sheet_board(SheetContext* ctx) {
    Canvas* canvas = ctx->canvas;
    Board* b = &g_board;
    *b = (Board){.font = ctx->font, .offset = ctx->offset};
    b->plane = canvas_add_layer(canvas, "in1.gnd");
    b->bottom = canvas_add_layer(canvas, "bottom");
    b->top = canvas_add_layer(canvas, "top");
    b->silk = canvas_add_layer(canvas, "silk");
    b->edge = canvas_add_layer(canvas, "edge");
    b->transient = canvas_add_layer(canvas, "transient");
    b->transient->above_active = true; // a ratsnest shows over whichever copper is active
    b->plane->opacity = 0.35f;
    b->bottom->opacity = 0.7f;
    b->top->opacity = 0.85f;
    b->plane_s = canvas_add_style(canvas, 0xb08a3cffu);
    b->bottom_s = canvas_add_style(canvas, 0x4d7fc4ffu);
    b->top_s = canvas_add_style(canvas, 0xc83434ffu);
    b->silk_s = canvas_add_style(canvas, 0xe6e6e6ffu);
    b->edge_s = canvas_add_style(canvas, 0xd8c75affu);
    b->wire_s = canvas_add_style(canvas, 0xffffffffu);

    // One net highlighted, raised over its neighbours and every other net
    // dimmed; one pad picked, outlined.
    CanvasTags* tags = canvas_tags(canvas);
    for (uint32_t net = 1; net <= 48; net++)
        canvas_tags_set(tags, net,
                        net == BOARD_HIGHLIGHT
                            ? (CanvasTagState){.colour = 0xffe08aff, .raised = true}
                            : (CanvasTagState){.dim = 0.35f});
    canvas_tags_set(tags, BOARD_PICKED, (CanvasTagState){.outline = 0xffffffffu});

    if (ctx->edit_thread && ctx->engine->idle) {
        // The thread is started late, and its work begun as it starts.
        g_late_edit = create_canvas_edit(canvas);
    } else if (ctx->edit_thread) {
        cetra_thread_t thread;
        if (cetra_thread_create(&thread, board_thread, ctx->edit))
            cetra_thread_join(thread);
        board_finish(canvas, ctx->edit);
    } else {
        board_fill(ctx->edit, b);
        board_finish(canvas, ctx->edit);
    }
    if (ctx->probe)
        g_after_draw = board_script;

    // Seen from below, a split's second view shows the plane, the bottom copper
    // and the outline, the bottom active; the first, the top active.
    canvas_layer_set_add(&ctx->under_shown, b->plane);
    canvas_layer_set_add(&ctx->under_shown, b->bottom);
    canvas_layer_set_add(&ctx->under_shown, b->edge);
    ctx->active[0] = b->top;
    ctx->active[1] = b->bottom;
}

/*
 * The strokes and dashes sheets (spec 14.22): white on black, a cell a case,
 * each stroked by its own paint, in millimetres. A cell with an area in closed
 * form prints its box, 0.6 mm each way, its area and its edge's length, in
 * pixels; a place that must be white or black prints the colour it wants; the
 * strings and the cells that must stay empty print their boxes alone.
 */
#define DASH_ON  0.2
#define DASH_OFF 0.2
#define DASH_W   0.05
#define CELL_MM  0.6

static const double DASH_TURN = 6.283185307179586;

static void cell_area(const SheetContext* ctx, const char* sheet, const char* name,
                      CanvasPoint centre, double area_mm2, double edge_mm) {
    if (!ctx->probe)
        return;
    const double px = ppu_px(ctx) * 1e6; // pixels a millimetre
    probe_cell(ctx, sheet, name, centre, MM(CELL_MM));
    printf(" area %.6f edge %.6f\n", area_mm2 * px * px, edge_mm * px);
}

static void dashes_box(const SheetContext* ctx, const char* kind, const char* name,
                       CanvasPoint centre) {
    if (!ctx->probe)
        return;
    probe_cell(ctx, kind, name, centre, MM(CELL_MM));
    printf("\n");
}

// The point `mm` millimetres from `o` at `angle` radians.
static CanvasPoint dashes_around(CanvasPoint o, double mm, double angle) {
    return (CanvasPoint){o.x + MM(mm * cos(angle)), o.y + MM(mm * sin(angle))};
}

// A polyline of two legs `width` wide meeting at `corner`, the first `in` long along +x into it
// and the second `out` long turning `turn` radians to the left out of it, its ends butt: its area
// is its legs' lengths times its width, less what its join takes off the mitre's point.
static void strokes_bend(const SheetContext* ctx, CanvasPaint paint, CanvasPoint corner, double in,
                         double out, double turn, double width) {
    const CanvasPoint legs[3] = {{corner.x - MM(in), corner.y},
                                 corner,
                                 {corner.x + MM(out * cos(turn)), corner.y + MM(out * sin(turn))}};
    paint.cap = CANVAS_CAP_BUTT;
    canvas_edit_polyline(
        ctx->edit,
        &(CanvasPolyline){.paint = paint, .points = legs, .count = 3, .width = MM(width)});
}

// What a join of `turn` radians takes off a mitre's point, in mm^2, a half width `h`: the kite
// between the two outer edges' ends and the point, less what the join keeps of it -- a bevel its
// triangle, a round join its sector. A mitre past four widths is drawn as a bevel.
static double strokes_cut(CanvasJoin join, double turn, double h) {
    const double kite = h * h * tan(turn * 0.5);
    if (join == CANVAS_JOIN_MITRE && cos(turn) >= -0.875)
        return 0.0;
    return kite - (join == CANVAS_JOIN_ROUND ? 0.5 * h * h * turn : 0.5 * h * h * sin(turn));
}

static void sheet_strokes(SheetContext* ctx) {
    CanvasEdit* edit = ctx->edit;
    const CanvasPaint white = white_paint(ctx->canvas);
    static const char* const CAPS[3] = {"round", "butt", "square"};
    static const char* const JOINS[3] = {"round", "mitre", "bevel"};
    char name[32];
#define CELL(i, j) \
    ((CanvasPoint){MM(-3.2 + 1.6 * (i)) + ctx->offset, MM(1.6 - 1.6 * (j)) + ctx->offset})
#define AT(c, dx, dy) ((CanvasPoint){(c).x + MM(dx), (c).y + MM(dy)})

    // A segment 0.8 long and 0.2 wide, and an arc of radius 0.4 through 1.5 radians 0.15 wide,
    // each capped three ways: a round cap adds a disc, a square one the width squared.
    for (int k = 0; k < 3; k++) {
        CanvasPaint paint = white;
        paint.cap = (CanvasCap)k;
        const double cap_area[3] = {DASH_TURN * 0.25, 0.0, 2.0}; // in half widths squared
        CanvasPoint c = CELL(k, 0);
        canvas_edit_segment(edit, &(CanvasSegment){.paint = paint,
                                                   .a = AT(c, -0.4, 0.0),
                                                   .b = AT(c, 0.4, 0.0),
                                                   .width = MM(0.2)});
        snprintf(name, sizeof(name), "segment-%s", CAPS[k]);
        cell_area(ctx, "strokes", name, c, 0.8 * 0.2 + cap_area[k] * 0.2 * 0.2 * 0.5,
                  2.0 * (0.8 + 0.2));
        c = k < 2 ? CELL(3 + k, 0) : CELL(0, 1);
        const CanvasPoint o = AT(c, -0.2, -0.2);
        canvas_edit_arc(edit, &(CanvasArc){.paint = paint,
                                           .a = AT(o, 0.4, 0.0),
                                           .b = AT(o, 0.4 * cos(1.5), 0.4 * sin(1.5)),
                                           .radius = MM(0.4),
                                           .width = MM(0.15)});
        snprintf(name, sizeof(name), "arc-%s", CAPS[k]);
        cell_area(ctx, "strokes", name, c, 0.4 * 1.5 * 0.15 + cap_area[k] * 0.15 * 0.15 * 0.5,
                  2.0 * (0.4 * 1.5 + 0.15));
    }

    // Two legs of 0.5 at a right angle and at 25 degrees, 0.15 wide, joined three ways: past four
    // widths the sharp one's mitre is its bevel.
    const double h = 0.075, sharp = DASH_TURN * 0.5 - 25.0 / 360.0 * DASH_TURN;
    for (int k = 0; k < 3; k++) {
        CanvasPaint paint = white;
        paint.join = (CanvasJoin)k;
        CanvasPoint c = CELL(1 + k, 1);
        strokes_bend(ctx, paint, AT(c, 0.2, -0.3), 0.5, 0.5, DASH_TURN * 0.25, 0.15);
        snprintf(name, sizeof(name), "right-%s", JOINS[k]);
        cell_area(ctx, "strokes", name, c, 0.15 - strokes_cut((CanvasJoin)k, DASH_TURN * 0.25, h),
                  2.0 * 1.15);
        c = k < 1 ? CELL(4, 1) : CELL(k - 1, 2);
        strokes_bend(ctx, paint, AT(c, 0.2, 0.0), 0.5, 0.5, sharp, 0.15);
        snprintf(name, sizeof(name), "sharp-%s", JOINS[k]);
        cell_area(ctx, "strokes", name, c, 0.15 - strokes_cut((CanvasJoin)k, sharp, h), 2.0 * 1.15);
    }
    // Just inside the right angle's mitre point, which a bevel cuts off.
    probe_pixel(ctx, "strokes-at", "mitre-point", AT(CELL(2, 1), 0.2 + 0.055, -0.3 - 0.055),
                0xffffff);
    probe_pixel(ctx, "strokes-at", "bevel-cut", AT(CELL(3, 1), 0.2 + 0.055, -0.3 - 0.055),
                0x000000);

    // A box 0.8 by 0.6 with square corners, stroked 0.1, joined three ways.
    for (int k = 0; k < 3; k++) {
        CanvasPaint paint = white;
        paint.join = (CanvasJoin)k;
        const CanvasPoint c = CELL(2 + k, 2);
        canvas_edit_box(edit, &(CanvasBox){.paint = paint,
                                           .centre = c,
                                           .size_x = MM(0.8),
                                           .size_y = MM(0.6),
                                           .stroke = MM(0.1)});
        const double corners[3] = {0.01 - DASH_TURN * 0.125 * 0.01, 0.0, 0.005};
        snprintf(name, sizeof(name), "box-%s", JOINS[k]);
        cell_area(ctx, "strokes", name, c, 0.28 - corners[k], 4.0 * (0.8 + 0.6));
    }

    // A segment 1 wide at 45 degrees, square capped, alone in its tile 20 mm off: its cap's corner
    // reaches the half width times the square root of two past its end along x, past the half
    // width alone. A view whose edge falls between the two draws the corner only if the tile's
    // bound reaches it.
    const CanvasPoint end = {MM(20.0) + ctx->offset, ctx->offset};
    CanvasPaint square = white;
    square.cap = CANVAS_CAP_SQUARE;
    canvas_edit_segment(
        edit,
        &(CanvasSegment){.paint = square, .a = AT(end, -2.0, -2.0), .b = end, .width = MM(1.0)});
    probe_pixel(ctx, "strokes-reach", "square-corner", AT(end, 0.64, 0.0), 0xffffff);
#undef AT
#undef CELL
}

static void sheet_dashes(SheetContext* ctx) {
    CanvasEdit* edit = ctx->edit;
    CanvasPaint paint = white_paint(ctx->canvas);
    const CanvasPaint solid = paint;
    paint.dash = (CanvasDash){MM(DASH_ON), MM(DASH_OFF), 0};
    paint.cap = CANVAS_CAP_BUTT;
    // The pattern half a gap on from a dash's end: a box's corners in the middle of gaps.
    CanvasPaint shifted = paint;
    shifted.dash.phase = MM(DASH_ON + DASH_OFF * 0.5);
    const double period = DASH_ON + DASH_OFF, w = DASH_W;
#define CELL(i, j) \
    ((CanvasPoint){MM(-3.25 + 1.3 * (i)) + ctx->offset, MM(2.25 - 1.5 * (j)) + ctx->offset})
#define AT(c, dx, dy) ((CanvasPoint){(c).x + MM(dx), (c).y + MM(dy)})

    // Three periods less a gap, butt ended: it ends on a dash's end.
    CanvasPoint c = CELL(0, 0);
    const CanvasSegment bar = {
        .paint = paint, .a = AT(c, -0.5, 0.0), .b = AT(c, 0.5, 0.0), .width = MM(w)};
    canvas_edit_segment(edit, &bar);
    cell_area(ctx, "dashes", "bar", c, 3.0 * DASH_ON * w, 6.0 * (DASH_ON + w));
    probe_pixel(ctx, "dashes-at", "bar-dash", AT(c, -0.5 + DASH_ON * 0.5, 0.0), 0xffffff);
    probe_pixel(ctx, "dashes-at", "bar-gap", AT(c, -0.5 + DASH_ON + DASH_OFF * 0.5, 0.0), 0x000000);

    // The same round: each dash a capsule, its ends past the dash's length.
    c = CELL(1, 0);
    CanvasSegment round_bar = bar;
    round_bar.paint.cap = CANVAS_CAP_ROUND;
    round_bar.a = AT(c, -0.5, 0.0), round_bar.b = AT(c, 0.5, 0.0);
    canvas_edit_segment(edit, &round_bar);
    cell_area(ctx, "dashes", "round-bar", c, 3.0 * (DASH_ON * w + DASH_TURN * w * w / 8.0),
              3.0 * (2.0 * DASH_ON + DASH_TURN * w * 0.5));

    // Two legs at a right angle, the corner in the middle of a gap and the far end in the middle
    // of a dash: the pattern carried round the corner. Begun again there, the second leg would
    // start on a dash.
    c = CELL(2, 0);
    const double leg1 = period + DASH_ON + DASH_OFF * 0.5,
                 leg2 = DASH_OFF * 0.5 + period + DASH_ON * 0.5;
    const CanvasPoint legs[3] = {AT(c, -0.35, -0.3), AT(c, -0.35 + leg1, -0.3),
                                 AT(c, -0.35 + leg1, -0.3 + leg2)};
    canvas_edit_polyline(
        edit, &(CanvasPolyline){.paint = paint, .points = legs, .count = 3, .width = MM(w)});
    cell_area(ctx, "dashes", "carried", c, 3.5 * DASH_ON * w, 7.0 * (DASH_ON + w));

    // The corner in the middle of a dash, mitred: that dash turns the corner square, its two
    // halves' lengths along the centreline times the width.
    c = CELL(3, 0);
    const double half_dash = DASH_ON * 0.5;
    const CanvasPoint bend[3] = {AT(c, -0.25, -0.25), AT(c, -0.25 + period + half_dash, -0.25),
                                 AT(c, -0.25 + period + half_dash, -0.25 + half_dash + period)};
    CanvasPaint mitred = paint;
    mitred.join = CANVAS_JOIN_MITRE;
    canvas_edit_polyline(
        edit, &(CanvasPolyline){.paint = mitred, .points = bend, .count = 3, .width = MM(w)});
    cell_area(ctx, "dashes", "corner", c, 3.0 * DASH_ON * w, 6.0 * (DASH_ON + w));

    // A ring, its pattern stretched to six whole periods round it.
    c = CELL(4, 0);
    canvas_edit_circle(
        edit, &(CanvasCircle){.paint = paint, .centre = c, .radius = MM(0.4), .stroke = MM(w)});
    const double round_mm = DASH_TURN * 0.4, periods = round(round_mm / period);
    const double on = round_mm / periods * DASH_ON / period, off = round_mm / periods - on;
    cell_area(ctx, "dashes", "ring", c, round_mm * w * DASH_ON / period, 2.0 * periods * (on + w));
    probe_pixel(ctx, "dashes-at", "ring-dash", dashes_around(c, 0.4, on * 0.5 / 0.4), 0xffffff);
    probe_pixel(ctx, "dashes-at", "ring-gap", dashes_around(c, 0.4, (on + off * 0.5) / 0.4),
                0x000000);

    // An arc two periods less a gap long, butt ended, counter-clockwise from a.
    c = CELL(5, 0);
    CanvasPoint o = AT(c, -0.25, -0.2);
    canvas_edit_arc(edit, &(CanvasArc){.paint = paint,
                                       .a = dashes_around(o, 0.5, 0.0),
                                       .b = dashes_around(o, 0.5, 1.2),
                                       .radius = MM(0.5),
                                       .width = MM(w)});
    cell_area(ctx, "dashes", "arc", c, 2.0 * DASH_ON * w, 4.0 * (DASH_ON + w));
    probe_pixel(ctx, "dashes-at", "arc-dash", dashes_around(o, 0.5, DASH_ON * 0.5 / 0.5), 0xffffff);
    probe_pixel(ctx, "dashes-at", "arc-gap",
                dashes_around(o, 0.5, (DASH_ON + DASH_OFF * 0.5) / 0.5), 0x000000);

    // A clockwise arc two periods long, its pattern from its own a: from its record's a the first
    // dash and gap would trade places.
    c = CELL(0, 1);
    o = AT(c, -0.2, -0.3);
    canvas_edit_arc(edit, &(CanvasArc){.paint = paint,
                                       .a = dashes_around(o, 0.5, 1.6),
                                       .b = dashes_around(o, 0.5, 0.0),
                                       .radius = MM(0.5),
                                       .width = MM(w),
                                       .clockwise = true});
    probe_pixel(ctx, "dashes-at", "back-dash", dashes_around(o, 0.5, 1.6 - DASH_ON * 0.5 / 0.5),
                0xffffff);
    probe_pixel(ctx, "dashes-at", "back-gap",
                dashes_around(o, 0.5, 1.6 - (DASH_ON + DASH_OFF * 0.5) / 0.5), 0x000000);

    // A box and a polygon over the same square, its sides two periods with its corners in the
    // middle of gaps, from the middle of the bottom side: eight dashes, two a side.
    c = CELL(1, 1);
    canvas_edit_box(
        edit,
        &(CanvasBox){
            .paint = shifted, .centre = c, .size_x = MM(0.8), .size_y = MM(0.8), .stroke = MM(w)});
    cell_area(ctx, "dashes", "box", c, 8.0 * DASH_ON * w, 16.0 * (DASH_ON + w));
    probe_pixel(ctx, "dashes-at", "box-dash", AT(c, 0.2, -0.4), 0xffffff);
    probe_pixel(ctx, "dashes-at", "box-gap", AT(c, 0.0, -0.4), 0x000000);
    c = CELL(2, 1);
    const CanvasPoint square[5] = {AT(c, 0.0, -0.4), AT(c, 0.4, -0.4), AT(c, 0.4, 0.4),
                                   AT(c, -0.4, 0.4), AT(c, -0.4, -0.4)};
    canvas_edit_polygon(
        edit, &(CanvasPolygon){.paint = shifted, .points = square, .count = 5, .stroke = MM(w)});
    cell_area(ctx, "dashes", "polygon", c, 8.0 * DASH_ON * w, 16.0 * (DASH_ON + w));
    probe_pixel(ctx, "dashes-at", "polygon-dash", AT(c, 0.2, -0.4), 0xffffff);
    probe_pixel(ctx, "dashes-at", "polygon-gap", AT(c, 0.0, -0.4), 0x000000);

    // Fills in a dashed paint, each its solid self.
    c = CELL(3, 1);
    canvas_edit_circle(edit, &(CanvasCircle){.paint = paint, .centre = c, .radius = MM(0.4)});
    cell_area(ctx, "dashes", "fill-circle", c, DASH_TURN * 0.5 * 0.16, DASH_TURN * 0.4);
    c = CELL(4, 1);
    canvas_edit_box(
        edit, &(CanvasBox){.paint = paint, .centre = c, .size_x = MM(0.8), .size_y = MM(0.6)});
    cell_area(ctx, "dashes", "fill-box", c, 0.48, 2.8);
    c = CELL(5, 1);
    const CanvasPoint triangle[3] = {AT(c, -0.4, -0.4), AT(c, 0.4, -0.4), AT(c, 0.0, 0.4)};
    canvas_edit_polygon(edit, &(CanvasPolygon){.paint = paint, .points = triangle, .count = 3});
    cell_area(ctx, "dashes", "fill-polygon", c, 0.32, 0.8 + 2.0 * hypot(0.4, 0.8));

    // One string plain and one in a dashed paint, a whole number of pixels apart, each within its
    // cell.
    for (int k = 0; k < 2; k++) {
        c = CELL(k, 2);
        canvas_edit_text(edit, &(CanvasText){.paint = k ? paint : solid,
                                             .font = ctx->font,
                                             .string = "Dash 0.5",
                                             .anchor = c,
                                             .size = MM(0.25),
                                             .align_x = CANVAS_ALIGN_CENTRE,
                                             .align_y = CANVAS_ALIGN_MIDDLE});
        dashes_box(ctx, "dashes-text", k ? "dashed" : "plain", c);
    }

    // The bar again in an object's colour; and in another object taken out in the same edit.
    CanvasTags* tags = canvas_tags(ctx->canvas);
    canvas_tags_set(tags, 7, (CanvasTagState){.colour = 0xff0000ffu});
    for (int k = 0; k < 2; k++) {
        c = CELL(2 + k, 2);
        CanvasSegment tagged = bar;
        tagged.paint.object = 7u + (uint32_t)k;
        tagged.a = AT(c, -0.5, 0.0), tagged.b = AT(c, 0.5, 0.0);
        canvas_edit_segment(edit, &tagged);
    }
    canvas_edit_remove_object(edit, 8);
    probe_pixel(ctx, "dashes-at", "tagged-dash", AT(CELL(2, 2), -0.5 + DASH_ON * 0.5, 0.0),
                0xff0000);
    dashes_box(ctx, "dashes-empty", "removed", CELL(3, 2));

    // A segment whose gap is negative and an arc whose dash is 2^30 units, each refused.
    c = CELL(4, 2);
    CanvasSegment backward = bar;
    backward.paint.dash.gap = -1;
    backward.a = AT(c, -0.5, 0.0), backward.b = AT(c, 0.5, 0.0);
    canvas_edit_segment(edit, &backward);
    CanvasPaint long_dash = paint;
    long_dash.dash.length = (int64_t)1 << 30;
    canvas_edit_arc(edit, &(CanvasArc){.paint = long_dash,
                                       .a = AT(c, 0.5, 0.0),
                                       .b = AT(c, 0.0, 0.5),
                                       .radius = MM(0.5),
                                       .width = MM(w)});
    dashes_box(ctx, "dashes-empty", "refused", c);

    // A mitred square polygon from a corner, in a pattern that fits round it 9.14 times and is
    // stretched to 9: its first point is where its last gap ends and its first dash begins, so no
    // join is drawn there, though its last segment comes to it a hair past a whole period.
    c = CELL(5, 2);
    CanvasPaint closing = paint;
    closing.dash = (CanvasDash){MM(0.2), MM(0.15), 0};
    closing.join = CANVAS_JOIN_MITRE;
    const CanvasPoint loop[4] = {AT(c, -0.4, -0.4), AT(c, 0.4, -0.4), AT(c, 0.4, 0.4),
                                 AT(c, -0.4, 0.4)};
    canvas_edit_polygon(
        edit, &(CanvasPolygon){.paint = closing, .points = loop, .count = 4, .stroke = MM(w)});
    const double closing_on = 3.2 * 0.2 / 0.35; // the dashes' length round it, stretched
    cell_area(ctx, "dashes", "closing", c, closing_on * w, 2.0 * (closing_on + 9.0 * w));
    probe_pixel(ctx, "dashes-at", "closing-corner", AT(c, -0.42, -0.405), 0x000000);

    // Two mitred legs 0.06 wide at a right angle, a dash ending 0.003 past the corner: inside the
    // turn, the first leg's body runs to the corner, though past the bisector, and the dash there
    // has turned it. Its dashes 0.103, 0.2 and 0.2 long, and the mitre's square outside the turn,
    // less where the 0.003 past the corner lies on the first leg's body.
    c = CELL(0, 3);
    CanvasPaint turned = mitred;
    turned.dash.phase = MM(0.097);
    const CanvasPoint notch[3] = {AT(c, -0.3, -0.3), AT(c, 0.2, -0.3), AT(c, 0.2, 0.2)};
    canvas_edit_polyline(
        edit, &(CanvasPolyline){.paint = turned, .points = notch, .count = 3, .width = MM(0.06)});
    cell_area(ctx, "dashes", "notch", c, 0.503 * 0.06 + 0.03 * (0.03 - 0.003),
              2.0 * (0.503 + 3.0 * 0.06));
    probe_pixel(ctx, "dashes-at", "notch-inside", AT(c, 0.2 - 0.012, -0.3 + 0.02), 0xffffff);

    // A mitred square-cornered box whose every corner ends a dash: no corner is drawn, its eight
    // dashes ending square at the corners.
    c = CELL(1, 3);
    CanvasPaint ended = mitred;
    ended.dash.phase = MM(DASH_ON);
    canvas_edit_box(
        edit,
        &(CanvasBox){
            .paint = ended, .centre = c, .size_x = MM(0.8), .size_y = MM(0.8), .stroke = MM(w)});
    cell_area(ctx, "dashes", "box-ends", c, 8.0 * DASH_ON * w, 16.0 * (DASH_ON + w));
    probe_pixel(ctx, "dashes-at", "box-corner", AT(c, 0.4 + w * 0.25, -0.4 - w * 0.25), 0x000000);

    // A solid bar crossed by a dashed one a quarter dash, raised and red, a dash's middle on the
    // crossing: the dashed bar's red there whether it is drawn dashed or, too fine, at its average.
    c = CELL(2, 3);
    canvas_edit_segment(
        edit, &(CanvasSegment){
                  .paint = solid, .a = AT(c, -0.5, 0.0), .b = AT(c, 0.5, 0.0), .width = MM(w)});
    canvas_tags_set(tags, 9, (CanvasTagState){.colour = 0xff0000ffu, .raised = true});
    CanvasPaint ghost = paint;
    ghost.object = 9;
    ghost.dash = (CanvasDash){MM(0.1), MM(0.3), MM(0.35)};
    canvas_edit_segment(
        edit, &(CanvasSegment){
                  .paint = ghost, .a = AT(c, 0.0, -0.5), .b = AT(c, 0.0, 0.5), .width = MM(w)});
    probe_pixel(ctx, "dashes-at", "raised-crossing", c, 0xff0000);
#undef AT
#undef CELL
}

typedef struct Sheet {
    const char* name;
    void (*build)(SheetContext* ctx);
    double centre_mm[2]; // where a view of it starts
    double width_mm;     // how much of it the window's width shows
    uint32_t background;
} Sheet;

static const Sheet SHEETS[] = {
    {"shapes", sheet_shapes, {-2.0, 0.5}, 24.0, 0x14181effu},
    {"areas", sheet_areas, {0.0, 0.0}, 8.0, 0x000000ffu},
    {"polygons", sheet_polygons, {0.0, 0.0}, 8.0, 0x000000ffu},
    {"ramp", sheet_ramp, {0.0, 0.0}, 8.0, 0x000000ffu},
    {"thin", sheet_thin, {0.0, 0.0}, 8.0, 0x000000ffu},
    // A view 10 micrometres across, centred on the line half a piece from a cut.
    {"long", sheet_long, {1.835008, 0.262144}, 0.01, 0x000000ffu},
    {"long-short", sheet_long_short, {1.835008, 0.262144}, 0.01, 0x000000ffu},
    {"symbol", sheet_symbol, {8.0, 1.0}, 48.0, 0xf5f4efffu},
    {"glyphs", sheet_glyphs, {0.0, 0.0}, 8.0, 0x000000ffu},
    {"overlap", sheet_overlap, {0.0, 0.0}, 8.0, 0x000000ffu},
    {"tags", sheet_tags, {0.0, 0.0}, 9.0, 0x000000ffu},
    {"dim", sheet_dim, {0.0, 0.0}, 9.0, 0x000000ffu},
    {"board", sheet_board, {0.0, 0.0}, 66.0, 0x001023ffu},
    {"strokes", sheet_strokes, {0.0, 0.0}, 8.0, 0x000000ffu},
    {"dashes", sheet_dashes, {0.0, 0.0}, 8.0, 0x000000ffu},
};

static const Sheet* find_sheet(const char* name) {
    for (size_t i = 0; i < sizeof(SHEETS) / sizeof(SHEETS[0]); i++)
        if (strcmp(SHEETS[i].name, name) == 0)
            return &SHEETS[i];
    return NULL;
}

/*
 * The window.
 */

// The column --history keeps at the window's right, in points; 0 without it.
static float g_side_width;

// The window's views, each an equal share of its width less the side column.
static void lay_out_views(const Engine* engine) {
    const float part = ((float)engine->win_width - g_side_width) / (float)g_view_count;
    for (int i = 0; i < g_view_count; i++) {
        g_views[i].x = part * (float)i;
        g_views[i].y = 0.0f;
        g_views[i].width = part;
        g_views[i].height = (float)engine->win_height;
    }
}

static int view_at(float x) {
    for (int i = g_view_count - 1; i > 0; i--)
        if (x >= g_views[i].x)
            return i;
    return 0;
}

static void draw_view(Engine* engine, void* user) {
    (void)user;
    lay_out_views(engine);
    if (g_probe_target) {
        printf("canvas frame target %s\n",
               postfx_picture_fbo(engine->postfx) ? "picture" : "window");
        fflush(stdout);
        g_probe_target = false;
    }
    // Under --budget, the draws' whole cost, GPU included: finished before the
    // clock starts and before it stops.
    double start = 0.0;
    if (g_budget) {
        glFinish();
        start = glfwGetTime();
    }
    for (int i = 0; i < g_view_count; i++)
        if (g_only_view < 0 || g_only_view == i)
            canvas_draw(g_canvas, &g_views[i]);
    if (g_budget) {
        glFinish();
        printf("canvas budget %d ms %.3f\n", engine->frame_count, (glfwGetTime() - start) * 1e3);
        fflush(stdout);
    }
    if (g_after_draw)
        g_after_draw();
}

// Under --idle: what is left of --idle-wake's argument, read an item a frame --
// its times joined by + asked on that frame -- and, headless, a line a frame and
// one at exit for the `idle` gate group.
static const char* g_wake_spec = "";
static bool g_idle_say;
static const char* g_idle_load; // --idle-load: a texture asked for as late work
static bool g_late_started;     // the late work has been started
static bool g_gui_click;        // --gui-click: a click handed to the debug GUI on the first frame
static UISystem* g_ui_enter;    // --ui-enter: the UI, given an input pass of nothing each frame
static bool g_probe;            // --canvas-probe: the run says what it did
static const char* g_ui_text;   // --ui-text: a label in the --ui panel
static UISystem* g_ui_live;     // the UI given the real pointer and keyboard each frame

// --ui-keys (spec 14.10): the --ui panel takes keys through a hook, as a pane an app draws would,
// and its label says the last two things it was handed. It takes every character and no key, so
// Tab and Esc reach the screen.
static bool g_ui_keys;
static UIElement* g_keys_label;
static char g_keys_said[2][32];

static bool panel_keys(UIElement* el, const InputKeyEvent* k, void* user) {
    (void)el;
    (void)user;
    memcpy(g_keys_said[0], g_keys_said[1], sizeof(g_keys_said[1]));
    if (k->codepoint) {
        char ch[5] = {0};
        text_utf8_encode(k->codepoint, ch);
        snprintf(g_keys_said[1], sizeof(g_keys_said[1]), "char %s", ch);
    } else {
        snprintf(g_keys_said[1], sizeof(g_keys_said[1]), "key %d mods %d", k->key, k->mods);
    }
    char text[80];
    snprintf(text, sizeof(text), "%s\n%s", g_keys_said[0], g_keys_said[1]);
    ui_set_text(g_keys_label, text);
    return k->codepoint != 0;
}

// --refuse-close (spec 14.7): the first close the window is asked for refused and said, as an app
// holding unsaved work would; the next taken. So one close was refused once one was asked.
static bool g_refuse_close;
static int g_close_asked;

static bool canvas_close(Engine* engine, void* user) {
    (void)engine;
    (void)user;
    if (g_close_asked++)
        return true;
    printf("canvas: the close was refused; ask again to close\n");
    fflush(stdout);
    return false;
}

// ---- The fonts built into the program (spec 14.6): the table canvas_fonts.h holds, which the
// build writes from its list, indexed by what each is for (CANVAS_FONTS_LABEL and the rest).

// A built-in font as a distance field in `pool`, at the size every font of the app is made at.
static Font* built_in_font(FontPool* pool, int which) {
    const CetraEmbeddedFile* f = &canvas_fonts[which];
    return load_font_memory(pool, f->source, f->bytes, f->size, 64.0f, true);
}

static GlyphCache* built_in_face(int which, float pixel_size) {
    return create_glyph_cache_memory(canvas_fonts[which].bytes, canvas_fonts[which].size,
                                     pixel_size);
}

// Two fonts the same: every glyph's metrics and place, the kerning table and the atlas.
static bool fonts_equal(const Font* a, const Font* b) {
    if (!a || !b || a->atlas_width != b->atlas_width || a->atlas_height != b->atlas_height ||
        a->kern_side != b->kern_side || a->em_size != b->em_size ||
        a->line_height != b->line_height || a->sdf_edge != b->sdf_edge ||
        HASH_COUNT(a->glyph_cache) != HASH_COUNT(b->glyph_cache))
        return false;
    for (const GlyphInfo* g = a->glyph_cache; g; g = g->hh.next) {
        const GlyphInfo* h = font_get_glyph(b, g->codepoint);
        if (!h || g->pair_row != h->pair_row || g->advance_x != h->advance_x ||
            g->left_bearing != h->left_bearing || g->x0 != h->x0 || g->y0 != h->y0 ||
            g->x1 != h->x1 || g->y1 != h->y1 || g->u0 != h->u0 || g->v0 != h->v0 ||
            g->u1 != h->u1 || g->v1 != h->v1)
            return false;
    }
    const size_t pairs = a->kern_side * a->kern_side * sizeof(int16_t);
    if ((a->kern_pairs == NULL) != (b->kern_pairs == NULL) ||
        (a->kern_pairs && memcmp(a->kern_pairs, b->kern_pairs, pairs)))
        return false;
    unsigned char *pa = atlas_of(a), *pb = atlas_of(b);
    const bool same = pa && pb && !memcmp(pa, pb, (size_t)a->atlas_width * (size_t)a->atlas_height);
    free(pa);
    free(pb);
    return same;
}

// Two editor faces the same: the same glyphs asked of each in turn land in the same places with
// the same bitmaps.
static bool faces_equal(GlyphCache* a, GlyphCache* b) {
    if (!a || !b || a->cell_width != b->cell_width || a->line_height != b->line_height ||
        a->ascent != b->ascent)
        return false;
    static const uint32_t MORE[] = {0x3a9, 0xb5, 0xe9, 0x2192};
    for (uint32_t i = 0; i < 95 + 4; i++) {
        const uint32_t cp = i < 95 ? 32 + i : MORE[i - 95];
        GlyphBitmap ga, gb;
        if (glyph_cache_get(a, cp, &ga) != glyph_cache_get(b, cp, &gb) || ga.left != gb.left ||
            ga.top != gb.top || ga.width != gb.width || ga.height != gb.height ||
            ga.missing != gb.missing || memcmp(ga.uv, gb.uv, sizeof(ga.uv)))
            return false;
    }
    return a->atlas.width == b->atlas.width && a->atlas.height == b->atlas.height &&
           !memcmp(a->pixels, b->pixels, (size_t)a->atlas.width * (size_t)a->atlas.height);
}

// Under --font-sheet --canvas-probe, run from the source root: each built-in font's bytes against
// the file it was built from, and a font and an editor face made from them against the same made
// from the file.
static void probe_built_in_fonts(void) {
    int bytes = 0, fonts = 0, faces = 0;
    for (int i = 0; i < CANVAS_FONTS_COUNT; i++) {
        const CetraEmbeddedFile* f = &canvas_fonts[i];
        long len = 0;
        char* file = read_entire_file(f->source, &len);
        bytes += file && (size_t)len == f->size && !memcmp(file, f->bytes, f->size);
        free(file);
        FontPool* by_path = create_font_pool();
        FontPool* by_bytes = create_font_pool();
        fonts +=
            fonts_equal(load_font(by_path, f->source, 64.0f, true), built_in_font(by_bytes, i));
        free_font_pool(by_path);
        free_font_pool(by_bytes);
        GlyphCache* a = create_glyph_cache(f->source, 13.0f);
        GlyphCache* b = built_in_face(i, 13.0f);
        faces += faces_equal(a, b);
        free_glyph_cache(a);
        free_glyph_cache(b);
    }
    printf("canvas fonts bytes %d fonts %d faces %d of %d\n", bytes, fonts, faces,
           CANVAS_FONTS_COUNT);
}

// ---- --font-sheet: the code editor's face (spec 14.4), as bitmaps made at the size shown and,
// under each, as the UI's distance field of the same face at the same size.

static const float FONT_SHEET_SIZES[] = {11.0f, 13.0f, 16.0f};
static const char* FONT_SHEET_TEXT =
    "(inst u1 tpd4e05 :value 4.7k) ; 0O 1lI \xce\xa9\xc2\xb5 \xc3\xa9";

typedef struct FontSheet {
    Engine* engine;
    GlyphCache* caches[3];
    Font* field; // the same face as a distance field
    Font* label;
} FontSheet;

static FontSheet g_font_sheet;

// One line from a glyph cache, its pen on whole framebuffer pixels: what the editor draws.
static void draw_bitmap_line(UIDrawList* dl, GlyphCache* c, float x, float baseline,
                             const char* text, float px_per_point, const vec4 color) {
    const float x_px = roundf(x * px_per_point), base_px = roundf(baseline * px_per_point);
    int col = 0;
    for (const char* p = text; *p; col++) {
        const uint32_t cp = text_utf8_next(&p);
        GlyphBitmap g;
        if (!glyph_cache_get(c, cp, &g) || !g.width)
            continue;
        const float gx = x_px + (float)(col * c->cell_width + g.left);
        const float gy = base_px + (float)g.top;
        const UIRect r = {gx / px_per_point, gy / px_per_point, (float)g.width / px_per_point,
                          (float)g.height / px_per_point};
        ui_draw_textured_quad_uv(dl, r, &c->atlas, g.uv, (float*)color);
    }
}

static void draw_font_sheet(UIElement* el, UIDrawList* dl, void* user) {
    FontSheet* s = user;
    const float px = (float)s->engine->fb_width / (float)s->engine->win_width;
    const vec4 ink = {0.86f, 0.87f, 0.84f, 1.0f};
    ui_draw_quad(dl, el->rect, (vec4){0.09f, 0.10f, 0.12f, 1.0f});
    float y = el->rect.y + 20.0f;
    for (int i = 0; i < 3; i++) {
        const float size = FONT_SHEET_SIZES[i];
        GlyphCache* c = s->caches[i];
        glyph_cache_set_size(c, size * px);
        char label[64];
        snprintf(label, sizeof(label), "%.0f pt: bitmap, then distance field", (double)size);
        ui_draw_text(dl, (UIRect){el->rect.x + 20.0f, y, 600.0f, 16.0f}, label,
                     &(UIStyle){.fg = {0.5f, 0.55f, 0.6f, 1.0f}, .font = s->label, .font_size = 12},
                     UI_ALIGN_START);
        y += 22.0f;
        draw_bitmap_line(dl, c, el->rect.x + 20.0f, y + (float)c->ascent / px, FONT_SHEET_TEXT, px,
                         ink);
        y += (float)c->line_height / px + 4.0f;
        ui_draw_text(
            dl, (UIRect){el->rect.x + 20.0f, y, 900.0f, size * 1.4f}, FONT_SHEET_TEXT,
            &(UIStyle){.fg = {ink[0], ink[1], ink[2], ink[3]}, .font = s->field, .font_size = size},
            UI_ALIGN_START);
        y += size * 1.4f + 18.0f;
    }
}

static bool build_font_sheet(UISystem* ui, Engine* engine, Font* label) {
    g_font_sheet.engine = engine;
    g_font_sheet.label = label;
    g_font_sheet.field = built_in_font(engine->text_renderer->font_pool, CANVAS_FONTS_EDITOR);
    for (int i = 0; i < 3; i++)
        g_font_sheet.caches[i] = built_in_face(CANVAS_FONTS_EDITOR, FONT_SHEET_SIZES[i]);
    if (!g_font_sheet.field || !g_font_sheet.caches[0] || !g_font_sheet.caches[1] ||
        !g_font_sheet.caches[2])
        return false;
    if (g_probe)
        probe_built_in_fonts();
    UIScreen* screen = ui_screen(ui, "font-sheet");
    UIElement* panel = ui_panel(ui_screen_root(screen));
    ui_set_size(panel, UI_GROW, 0.0f, UI_GROW, 0.0f);
    ui_set_style(panel, &(UIStyle){.bg = {0.09f, 0.10f, 0.12f, 1.0f}});
    ui_set_draw(panel, draw_font_sheet, &g_font_sheet);
    ui_push(ui, screen);
    return true;
}

/*
 * --editor FILE: the code editor (spec 14.4) over the whole window, on a file's text, with what
 * pcb would hand it standing in for pcb:
 * - an S-expression highlighter on pcb's lexical rules, a line at a time;
 * - a "parse" on a thread of its own, its brackets and strings checked, whose errors come back a
 *   few versions behind and are underlined where the typing since has moved them;
 * - F6, an edit landed at the text's start as another view's would be;
 * - F7, the file read again through the text's whole new text.
 * A parse's result is taken on the frame its thread's end wakes; --editor-lag N takes it N frames
 * after it began instead, waiting for it then, so a headless run repeats without idle mode too,
 * whose frames do not wait for work as an idle run's do (spec 14.7). --vim puts the vim mode
 * (spec 14.5) under the app's keys, with its state on a line under the editor. --read-only makes
 * every file's text read-only (spec 14.7): nothing typed changes it, and each refusal is counted
 * and, with --vim, said on that line.
 *
 * --editor may be given more than once, and the editors are PANES (spec 14.6), a test bed for
 * taking elements out, moving them and showing an editor another text -- not pcb's panes, whose
 * keys are pcb's:
 * - F8 splits the focused pane: a row takes its place, it moves into the row, and a second editor
 *   on the same text, with carets of its own, goes beside it;
 * - F9 closes the focused pane, the focus going to the pane left in its place; a row left holding
 *   one pane hands it to the row's own parent;
 * - F10 shows the next file in the focused pane, where that pane last was in it.
 */

enum {
    STYLE_COMMENT = 1,
    STYLE_STRING,
    STYLE_KEYWORD,
    STYLE_NUMBER,
    STYLE_BRACKET,
    STYLE_HEAD,
    STYLE_ERROR,
    STYLE_LANDED,
};

#define PARSE_ERRORS_MAX 64

typedef struct ParseJob {
    char* text;
    size_t len;
    uint64_t version;
    TextRange errors[PARSE_ERRORS_MAX];
    size_t error_count;
    cetra_thread_t thread;
    bool running;
    size_t started;   // the frame it began on
    atomic_bool done; // the thread has finished, and ended its work with the engine
    Engine* engine;
} ParseJob;

#define EDITOR_FILES_MAX 8

// A file the editors show: its text, and the parse that reads it.
typedef struct EditorFile {
    const char* path;
    TextDoc* doc;
    ParseJob job;
    uint64_t parsed; // the version the last result was for; UINT64_MAX for none
} EditorFile;

// The panes are the editors in the UI's tree, under `area`, side by side in rows: nothing beside
// the tree says where they are, which file each shows (its text says), or where each was in a file
// it left (the editor keeps that).
typedef struct EditorSheet {
    EditorFile files[EDITOR_FILES_MAX];
    int file_count;
    UIElement* area; // the panes' container, a row
    GlyphCache* face;
    UIScreen* screen;
    int lag;        // frames from a parse's start to its result being taken
    bool read_only; // every file's text read-only
    int refused;    // what the texts refused, being read-only
    bool lookup;    // --vim-lookup: the vim mode's lookups answered, saying what they asked
    bool layout;    // --vim-layout: = and gq and new lines' indents answered
    int fold_first, fold_last; // --editor-fold: the first pane's lines folded, as numbered; 0 none
    int filler_line, filler_rows; // --editor-filler: blank rows above a line, as numbered; 0 none
    // With --vim: the vim mode under the app's own, and a line under the editor saying its state,
    // with the line / ? and : are typed in after it -- its kind shown before it -- and what the
    // last such line came to, said until the next key; and the editor that asked for the line.
    UIEditorVim* vim;
    UIElement* status;
    UIElement* prompt_kind;
    UIElement* prompt;
    UIElement* asking;
    char kind;
    char message[160];
    char status_text[192];
} EditorSheet;

static EditorSheet g_editor_sheet;

// The file the editor shows, by its text; NULL for none.
static EditorFile* file_of(const UIElement* editor) {
    const TextDoc* doc = ui_editor_doc(editor);
    for (int i = 0; doc && i < g_editor_sheet.file_count; i++)
        if (g_editor_sheet.files[i].doc == doc)
            return &g_editor_sheet.files[i];
    return NULL;
}

static int panes_under(const UIElement* el) {
    if (el->kind == UI_EDITOR)
        return 1;
    int n = 0;
    for (size_t i = 0; i < el->child_count; i++)
        n += panes_under(el->children[i]);
    return n;
}

// The first pane under `el`, in the window's order.
static UIElement* pane_first(UIElement* el) {
    while (el->kind != UI_EDITOR)
        el = el->children[0];
    return el;
}

// The pane holding the focus; the one that asked for the vim line while the line has it; or the
// first.
static UIElement* pane_focused(void) {
    UIElement* f = ui_focused(g_editor_sheet.screen);
    if (f && f->kind == UI_EDITOR)
        return f;
    return g_editor_sheet.asking ? g_editor_sheet.asking : pane_first(g_editor_sheet.area);
}

// A pane's editor on a text, in the app's keys.
static UIElement* pane_open(UIElement* parent, TextDoc* doc);

// A row of panes, with no padding of its own, so one pane in it stands where the row does.
static UIElement* pane_row(UIElement* parent) {
    UIElement* row = ui_panel(parent);
    row->dir = UI_ROW;
    ui_set_size(row, UI_GROW, 0.0f, UI_GROW, 0.0f);
    for (int k = 0; k < 4; k++)
        row->padding[k] = -1.0f;
    row->spacing = 4.0f;
    return row;
}

// A byte that ends an atom, on pcb's rules: whitespace, a bracket, a quote or a comment's start.
static bool sexpr_stop(char c) {
    return c == ' ' || c == '\t' || c == '(' || c == ')' || c == '"' || c == ';';
}

// A line coloured on pcb's lexical rules: a comment from ';' to the line's end, a string on one
// line with its escapes, brackets, an atom after '(' as the form's head, `:keywords` and numbers.
static size_t sexpr_line(const char* line, size_t len, uint32_t* state, TextSpan* spans, size_t max,
                         void* user) {
    (void)user;
    size_t n = 0;
    bool head = false;
    for (size_t i = 0; i < len;) {
        const char c = line[i];
        size_t j = i + 1;
        uint16_t style = 0;
        if (c == ';') {
            j = len;
            style = STYLE_COMMENT;
        } else if (c == '"') {
            while (j < len && line[j] != '"')
                j += line[j] == '\\' ? 2 : 1;
            j = j < len ? j + 1 : len;
            style = STYLE_STRING;
        } else if (c == '(' || c == ')') {
            style = STYLE_BRACKET;
        } else if (c == ' ' || c == '\t') {
            i++;
            continue;
        } else {
            j = i;
            while (j < len && !sexpr_stop(line[j]))
                j++;
            const bool digit = (c >= '0' && c <= '9') || ((c == '-' || c == '.') && i + 1 < len &&
                                                          line[i + 1] >= '0' && line[i + 1] <= '9');
            style = c == ':' ? STYLE_KEYWORD : digit ? STYLE_NUMBER : head ? STYLE_HEAD : 0;
        }
        if (style && n < max)
            spans[n] = (TextSpan){(uint32_t)i, (uint32_t)(j - i), style};
        n += style != 0;
        head = c == '(';
        i = j;
    }
    *state = 0;
    return n;
}

// The parse: brackets left open or closing nothing, and a string its line ends inside.
static void* parse_thread(void* arg) {
    ParseJob* job = arg;
    size_t open[256];
    size_t depth = 0;
    job->error_count = 0;
    for (size_t i = 0; i < job->len; i++) {
        const char c = job->text[i];
        if (c == ';') {
            while (i < job->len && job->text[i] != '\n')
                i++;
        } else if (c == '"') {
            size_t j = i + 1;
            while (j < job->len && job->text[j] != '"' && job->text[j] != '\n')
                j += job->text[j] == '\\' ? 2 : 1;
            if (j >= job->len || job->text[j] != '"') {
                if (job->error_count < PARSE_ERRORS_MAX)
                    job->errors[job->error_count++] = (TextRange){i, j, STYLE_ERROR};
            }
            i = j;
        } else if (c == '(') {
            if (depth < 256)
                open[depth] = i;
            depth++;
        } else if (c == ')') {
            if (depth)
                depth--;
            else if (job->error_count < PARSE_ERRORS_MAX)
                job->errors[job->error_count++] = (TextRange){i, i + 1, STYLE_ERROR};
        }
    }
    for (size_t d = 0; d < depth && d < 256 && job->error_count < PARSE_ERRORS_MAX; d++)
        job->errors[job->error_count++] = (TextRange){open[d], open[d] + 1, STYLE_ERROR};
    atomic_store(&job->done, true);
    engine_end_work(job->engine);
    return NULL;
}

// The vim mode's state on the line under the editor, as vim's own status line shows it, with the
// keys of a command half typed at its end -- less the line's own kind while a line is typed, which
// stands before the field -- or what the last line typed came to.
static void editor_vim_status(EditorSheet* s) {
    if (!s->vim || !s->status)
        return;
    static const char* const SHOWN[] = {"", "-- INSERT --", "-- VISUAL --", "-- VISUAL LINE --",
                                        "-- VISUAL BLOCK --"};
    char text[sizeof(s->status_text)];
    const char* pending = ui_editor_vim_pending(s->vim);
    int keys = (int)strlen(pending);
    if (s->kind && keys && pending[keys - 1] == s->kind)
        keys--;
    if (s->message[0] && !s->kind)
        snprintf(text, sizeof(text), "%s", s->message);
    else
        snprintf(text, sizeof(text), "%-20s %.*s", SHOWN[ui_editor_vim_state(s->vim)], keys,
                 pending);
    if (strcmp(text, s->status_text)) {
        memcpy(s->status_text, text, sizeof(text));
        ui_set_text(s->status, text);
    }
}

// The line / ? and : are typed in, done with: emptied, its kind put away, the editor that asked for
// it focused again.
static void editor_prompt_close(EditorSheet* s) {
    s->kind = 0;
    ui_set_text(s->prompt_kind, "");
    ui_field_set_text(s->prompt, "");
    ui_set_focus(s->screen, s->asking);
}

// The vim mode asking for a line: the field under the editor focused, its kind before it.
static void editor_prompt_open(UIElement* el, char kind, void* user) {
    EditorSheet* s = user;
    const char shown[2] = {kind, '\0'};
    s->asking = el;
    s->kind = kind;
    s->message[0] = '\0';
    ui_set_text(s->prompt_kind, shown);
    ui_field_set_text(s->prompt, "");
    ui_set_focus(s->screen, s->prompt);
}

// A search's matches lit as it is typed.
static void editor_prompt_change(UIElement* el, void* user) {
    const EditorSheet* s = user;
    if (s->kind == '/' || s->kind == '?')
        ui_editor_vim_light(s->vim, s->asking, ui_field_text(el));
}

// Enter: the line handed to the vim mode, and what it came to said when it found nothing. The line
// is copied whole first, since closing empties the field.
static void editor_prompt_submit(UIElement* el, void* user) {
    EditorSheet* s = user;
    char* text = strdup(ui_field_text(el));
    if (!text)
        return;
    const char kind = s->kind;
    editor_prompt_close(s);
    if (kind == ':' && !ui_editor_vim_command(s->vim, s->asking, text))
        snprintf(s->message, sizeof(s->message), "Not an editor command: %s", text);
    else if (kind != ':' && !ui_editor_vim_search(s->vim, s->asking, text, kind == '?'))
        snprintf(s->message, sizeof(s->message), "Pattern not found: %s", text);
    free(text);
}

// Esc in the line: let go, the vim mode told.
static bool editor_prompt_key(UIElement* el, const InputKeyEvent* key, void* user) {
    (void)el;
    EditorSheet* s = user;
    if (key->key != GLFW_KEY_ESCAPE)
        return false;
    const char kind = s->kind;
    editor_prompt_close(s);
    if (kind == ':')
        ui_editor_vim_command(s->vim, s->asking, NULL);
    else
        ui_editor_vim_search(s->vim, s->asking, NULL, kind == '?');
    return true;
}

// Once a frame for each file: a parse begun on a text that changed, and its result taken, at the
// version it read, on the frame its end wakes -- or `lag` frames after it began, waiting for it
// then.
static void editor_parse_frame(Engine* engine, EditorFile* f, int lag) {
    ParseJob* job = &f->job;
    const bool due =
        lag > 0 ? engine->total_frames - job->started >= (size_t)lag : atomic_load(&job->done);
    if (job->running && due) {
        cetra_thread_join(job->thread);
        job->running = false;
        text_doc_set_ranges(f->doc, 0, job->version, job->errors, job->error_count);
        f->parsed = job->version;
        free(job->text);
        job->text = NULL;
    }
    if (!job->running && f->parsed != f->doc->version) {
        job->text = text_doc_text(f->doc, &job->len);
        job->version = f->doc->version;
        job->started = engine->total_frames;
        job->engine = engine;
        atomic_store(&job->done, false);
        engine_begin_work(engine);
        job->running = job->text && cetra_thread_create(&job->thread, parse_thread, job);
        if (!job->running) {
            engine_end_work(engine);
            free(job->text);
            job->text = NULL;
        }
    }
}

static void editor_host_frame(Engine* engine) {
    EditorSheet* s = &g_editor_sheet;
    editor_vim_status(s);
    for (int i = 0; i < s->file_count; i++)
        editor_parse_frame(engine, &s->files[i], s->lag);
}

// F6: an edit made in another view landed at the start of the text the editor shows, marked where
// it landed.
static bool host_land(UIElement* el, void* user) {
    (void)user;
    TextDoc* doc = ui_editor_doc(el);
    if (!doc)
        return false;
    static const char LANDED[] = "(note landed)\n";
    text_doc_replace(doc, 0, 0, LANDED, sizeof(LANDED) - 1);
    const TextRange mark = {0, sizeof(LANDED) - 2, STYLE_LANDED};
    text_doc_set_ranges(doc, 1, doc->version, &mark, 1);
    return true;
}

// F7: the editor's file read again, as after an undo on pcb's timeline wrote it.
static bool host_reload(UIElement* el, void* user) {
    (void)user;
    const EditorFile* f = file_of(el);
    long len = 0;
    char* bytes = f ? read_entire_file(f->path, &len) : NULL;
    if (!bytes)
        return false;
    text_doc_set_text(f->doc, bytes, (size_t)len);
    free(bytes);
    return true;
}

// --split-share F: the first side of a split takes a share of F of the row, the second the rest; 0
// both share it evenly.
static float g_split_share;

// `to` sized as `from` is, for an element taking another's place in its parent.
static void take_size(UIElement* to, const UIElement* from) {
    ui_set_size(to, from->size_mode[0], from->size[0], from->size_mode[1], from->size[1]);
}

// F8: the pane split. A row takes its place and its size, it moves into the row, and a second
// editor on the same text goes beside it, with carets of its own and the focus.
static bool host_split(UIElement* el, void* user) {
    const EditorSheet* s = user;
    UIElement* row = pane_row(el->parent);
    take_size(row, el);
    ui_move(row, el->parent, el);
    ui_move(el, row, NULL);
    ui_set_size(el, g_split_share > 0.0f ? UI_SHARE : UI_GROW, g_split_share, UI_GROW, 0.0f);
    ui_set_focus(s->screen, pane_open(row, ui_editor_doc(el)));
    return true;
}

// F9: the pane closed, from inside its own key, and the focus given to the pane beside it, which
// takes its place -- as is the vim line when the pane had asked for it. A row left holding one pane
// hands it to the row's own parent, in the row's place and size. The last pane stays.
static bool host_close(UIElement* el, void* user) {
    EditorSheet* s = user;
    if (panes_under(s->area) < 2)
        return false;
    UIElement* parent = el->parent;
    UIElement* beside = NULL;
    for (size_t i = 0; i < parent->child_count; i++)
        if (parent->children[i] == el)
            beside = parent->children[i + 1 < parent->child_count ? i + 1 : i - 1];
    ui_remove(el);
    if (parent != s->area && parent->child_count == 1) {
        take_size(beside, parent);
        ui_move(beside, parent->parent, parent);
        ui_remove(parent);
    }
    UIElement* next = pane_first(beside);
    ui_set_focus(s->screen, next);
    if (s->asking == el)
        s->asking = next;
    return true;
}

// F10: the next file in the pane, which comes back where it last was in it.
static bool host_next_file(UIElement* el, void* user) {
    const EditorSheet* s = user;
    const EditorFile* f = file_of(el);
    if (s->file_count < 2 || !f)
        return false;
    ui_editor_set_doc(el, s->files[(f - s->files + 1) % s->file_count].doc);
    return true;
}

// A change a read-only text refused: counted, and said until the next key, as vim says E21.
static void editor_refused(TextDoc* doc, const UIElement* editor, void* user) {
    (void)doc;
    (void)editor;
    EditorSheet* s = user;
    s->refused++;
    snprintf(s->message, sizeof(s->message), "Cannot make changes, the text is read only");
}

// --vim-lookup: a host standing in for pcb's reader, which says on the vim line what it was asked
// and the name it was asked about -- the atom at the caret on pcb's rules, or a visual area's text
// -- and answers that it went there.
static bool editor_lookup(UIElement* el, UIEditorVimLookup kind, void* user) {
    static const char* const ASKED[] = {"gd", "gD", "Ctrl+]", "K"};
    EditorSheet* s = user;
    TextDoc* doc = ui_editor_doc(el);
    const TextSelection* sel = NULL;
    size_t primary = 0;
    if (!doc || !ui_editor_selections(el, &sel, &primary))
        return false;
    const TextSelection at = sel[primary];
    const size_t line = text_doc_line_of(doc, at.head), start = text_doc_line_start(doc, line);
    size_t len = 0;
    const char* text = text_doc_line(doc, line, &len);
    size_t lo = (at.anchor < at.head ? at.anchor : at.head) - start;
    size_t hi = (at.anchor < at.head ? at.head : at.anchor) - start;
    if (at.anchor == at.head) {
        while (lo > 0 && !sexpr_stop(text[lo - 1]))
            lo--;
        while (hi < len && !sexpr_stop(text[hi]))
            hi++;
    } else if (hi < len) {
        // A visual area holds the character at its far end.
        hi++;
        while (hi < len && text_utf8_is_continuation((unsigned char)text[hi]))
            hi++;
    }
    snprintf(s->message, sizeof(s->message), "%s %.*s", ASKED[kind], (int)(hi - lo), text + lo);
    return true;
}

// The brackets a line opens less those it closes.
static int bracket_depth(const char* s, size_t len) {
    int open = 0;
    for (size_t i = 0; i < len; i++)
        open += (s[i] == '(') - (s[i] == ')');
    return open;
}

// --vim-layout: a host's indent for an S-expression, two spaces for every bracket still open where
// a line starts -- brackets in comments and strings counted too, as a sketch of pcb's reader --
// with a line that opens with a closing bracket one level less. The brackets above the lines are
// counted once, and then down them.
static bool editor_indent(UIElement* el, size_t first, size_t last, int* cols, void* user) {
    (void)user;
    TextDoc* doc = ui_editor_doc(el);
    if (!doc)
        return false;
    int open = 0;
    size_t len = 0;
    for (size_t l = 0; l < first; l++)
        open += bracket_depth(text_doc_line(doc, l, &len), len);
    for (size_t line = first; line <= last; line++) {
        const char* s = text_doc_line(doc, line, &len);
        size_t i = 0;
        while (i < len && (s[i] == ' ' || s[i] == '\t'))
            i++;
        const int depth = open - (i < len && s[i] == ')');
        cols[line - first] = depth > 0 ? 2 * depth : 0;
        open += bracket_depth(s, len);
    }
    return true;
}

// --vim-layout: the lines' words joined on one line at the first line's indent.
static bool editor_format(UIElement* el, size_t first, size_t last, char** out, size_t* len,
                          void* user) {
    (void)user;
    TextDoc* doc = ui_editor_doc(el);
    if (!doc)
        return false;
    size_t bytes = 0;
    for (size_t l = first; l <= last; l++) {
        size_t ll = 0;
        text_doc_line(doc, l, &ll);
        bytes += ll + 1;
    }
    char* buf = malloc(bytes + 1);
    if (!buf)
        return false;
    size_t n = 0;
    for (size_t l = first; l <= last; l++) {
        size_t ll = 0;
        const char* s = text_doc_line(doc, l, &ll);
        size_t i = 0;
        // The first line keeps its indent; every other line's is dropped with its break.
        if (l == first) {
            while (i < ll && (s[i] == ' ' || s[i] == '\t'))
                buf[n++] = s[i++];
        }
        for (; i < ll; i++) {
            const bool blank = s[i] == ' ' || s[i] == '\t';
            if (!blank)
                buf[n++] = s[i];
            else if (n && buf[n - 1] != ' ' && i + 1 < ll)
                buf[n++] = ' ';
        }
        if (l < last && n && buf[n - 1] != ' ')
            buf[n++] = ' ';
    }
    while (n && buf[n - 1] == ' ')
        n--;
    *out = buf;
    *len = n;
    return true;
}

// Any key in the editor: what the last line came to is said no longer; the key goes on.
static bool host_any_key(UIElement* el, const InputKeyEvent* key, void* user) {
    (void)el;
    (void)key;
    (void)user;
    g_editor_sheet.message[0] = '\0';
    return false;
}

static const UIEditorCommand HOST_LAND = {"land-edit", "Land an edit from another view", host_land,
                                          NULL};
static const UIEditorCommand HOST_RELOAD = {"reload", "Read the file again", host_reload, NULL};
static const UIEditorCommand HOST_SPLIT = {"split", "Split the pane", host_split, &g_editor_sheet};
static const UIEditorCommand HOST_CLOSE = {"close", "Close the pane", host_close, &g_editor_sheet};
static const UIEditorCommand HOST_NEXT_FILE = {"next-file", "Show the next file in the pane",
                                               host_next_file, &g_editor_sheet};
static const UIEditorBinding HOST_KEYS[] = {
    {GLFW_KEY_F6, 0, &HOST_LAND},       {GLFW_KEY_F7, 0, &HOST_RELOAD},
    {GLFW_KEY_F8, 0, &HOST_SPLIT},      {GLFW_KEY_F9, 0, &HOST_CLOSE},
    {GLFW_KEY_F10, 0, &HOST_NEXT_FILE},
};
static UIEditorMode g_host_mode = {.name = "pcb",
                                   .bindings = HOST_KEYS,
                                   .binding_count = sizeof(HOST_KEYS) / sizeof(HOST_KEYS[0]),
                                   .read_key = host_any_key,
                                   .layer = true};

static UIElement* pane_open(UIElement* parent, TextDoc* doc) {
    UIElement* ed = ui_editor(parent, doc);
    ui_editor_set_mode(ed, &g_host_mode);
    return ed;
}

// The editor over the window, on the files given, at most EDITOR_FILES_MAX; false, said by name,
// when it cannot be.
static bool build_editor(UISystem* ui, const char* const* paths, int count, bool vim) {
    EditorSheet* s = &g_editor_sheet;
    for (int i = 0; i < count; i++) {
        EditorFile* f = &s->files[s->file_count];
        long len = 0;
        char* bytes = read_entire_file(paths[i], &len);
        f->path = paths[i];
        f->doc = bytes ? create_text_doc(bytes, (size_t)len) : NULL;
        f->parsed = UINT64_MAX;
        free(bytes);
        if (!f->doc) {
            fprintf(stderr, "canvas: --editor could not open %s\n", paths[i]);
            return false;
        }
        text_doc_set_highlighter(f->doc, sexpr_line, NULL, false);
        f->doc->read_only = s->read_only;
        text_doc_set_on_refused(f->doc, editor_refused, s);
        s->file_count++;
    }
    s->face = built_in_face(CANVAS_FONTS_EDITOR, 13.0f);
    if (!s->face) {
        fprintf(stderr, "canvas: --editor could not make the editor's face\n");
        return false;
    }
    UITheme theme = *ui_theme(ui);
    theme.editor_face = s->face;
    theme.editor_size = 13.0f;
    UIEditorStyle* st = theme.editor_styles;
    glm_vec4_copy((vec4){0.45f, 0.55f, 0.45f, 1.0f}, st[STYLE_COMMENT].text);
    glm_vec4_copy((vec4){0.82f, 0.71f, 0.47f, 1.0f}, st[STYLE_STRING].text);
    glm_vec4_copy((vec4){0.55f, 0.75f, 0.95f, 1.0f}, st[STYLE_KEYWORD].text);
    glm_vec4_copy((vec4){0.85f, 0.62f, 0.82f, 1.0f}, st[STYLE_NUMBER].text);
    glm_vec4_copy((vec4){0.55f, 0.56f, 0.62f, 1.0f}, st[STYLE_BRACKET].text);
    glm_vec4_copy((vec4){0.95f, 0.76f, 0.45f, 1.0f}, st[STYLE_HEAD].text);
    glm_vec4_copy((vec4){0.95f, 0.30f, 0.30f, 1.0f}, st[STYLE_ERROR].underline);
    glm_vec4_copy((vec4){0.30f, 0.65f, 0.35f, 0.30f}, st[STYLE_LANDED].background);
    ui_set_theme(ui, &theme);
    UIScreen* screen = ui_screen(ui, "editor");
    ui_screen_set_modal(screen, false);
    s->screen = screen;
    UIElement* root = ui_screen_root(screen);
    // The panes' row, so one pane stands where the editor alone did.
    s->area = pane_row(root);
    UIElement* first = pane_open(s->area, s->files[0].doc);
    if (s->fold_first > 0 && s->fold_last >= s->fold_first)
        ui_editor_set_folds(
            first, &(UIEditorFold){(size_t)s->fold_first - 1, (size_t)s->fold_last - 1}, 1);
    if (s->filler_line > 0 && s->filler_rows > 0)
        ui_editor_set_fillers(
            first, &(UIEditorFiller){(size_t)s->filler_line - 1, (uint32_t)s->filler_rows}, 1);
    g_host_mode.parent = ui_editor_standard_mode();
    if (vim) {
        s->vim = create_ui_editor_vim();
        if (!s->vim) {
            fprintf(stderr, "canvas: --vim could not make the vim mode\n");
            return false;
        }
        g_host_mode.parent = ui_editor_vim_mode(s->vim);
        ui_editor_vim_set_prompt(s->vim, editor_prompt_open, s);
        if (s->lookup)
            ui_editor_vim_set_lookup(s->vim, editor_lookup, s);
        if (s->layout) {
            ui_editor_vim_set_indent(s->vim, editor_indent, s);
            ui_editor_vim_set_format(s->vim, editor_format, s);
        }
        root->align_cross = UI_ALIGN_START;
        // Under the editor: its state, then the line / ? and : are typed in, after its kind.
        UIElement* row = ui_panel(root);
        row->dir = UI_ROW;
        ui_set_size(row, UI_GROW, 0.0f, UI_FIT, 0.0f);
        row->align_cross = UI_ALIGN_CENTER;
        s->status = ui_label(row, "");
        ui_set_size(s->status, UI_FIXED, 320.0f, UI_FIT, 0.0f);
        s->prompt_kind = ui_label(row, "");
        ui_set_size(s->prompt_kind, UI_FIXED, 14.0f, UI_FIT, 0.0f);
        s->prompt = ui_text_field(row, 0, editor_prompt_submit, s);
        ui_set_size(s->prompt, UI_GROW, 0.0f, UI_FIT, 0.0f);
        ui_field_set_on_change(s->prompt, editor_prompt_change, s);
        ui_set_on_key(s->prompt, editor_prompt_key, s);
        editor_vim_status(s);
    }
    ui_push(ui, screen);
    ui_set_focus(screen, first);
    return true;
}

// "canvas <what> <the text, as hex>".
// "canvas <what> <bytes as hex>" for the probe; NULL bytes print none.
static void say_hex(const char* what, const char* bytes, size_t len) {
    printf("canvas %s ", what);
    for (size_t i = 0; bytes && i < len; i++)
        printf("%02x", (unsigned char)bytes[i]);
    printf("\n");
}

static void say_text(const char* what, const TextDoc* doc) {
    size_t len = 0;
    char* text = text_doc_text(doc, &len);
    say_hex(what, text, len);
    free(text);
}

// " <count> <anchor> <head> ..." for an editor's selections, the line left open.
static void say_selections(const UIElement* editor) {
    const TextSelection* s = NULL;
    const size_t n = ui_editor_selections(editor, &s, NULL);
    printf(" %zu", n);
    for (size_t i = 0; i < n; i++)
        printf(" %zu %zu", s[i].anchor, s[i].head);
}

// "canvas pane <k> file <f> top <offset> focused <0 or 1> sel <selections>" for each pane under
// `el`, in the window's order.
static void say_panes(const UIElement* el, int* k, const UIElement* focused) {
    if (el->kind == UI_EDITOR) {
        const EditorFile* f = file_of(el);
        printf("canvas pane %d file %d top %zu focused %d sel", (*k)++,
               f ? (int)(f - g_editor_sheet.files) : -1, ui_editor_top(el), el == focused);
        say_selections(el);
        printf("\n");
        return;
    }
    for (size_t i = 0; i < el->child_count; i++)
        say_panes(el->children[i], k, focused);
}

// The panes' tree under `el` with no spaces, each pane its number in the window's order and each
// row its children in parentheses: "((0,(1,2)))". *asked is the number of the pane `asking`, when
// it is one, the pane the vim line answers.
static void say_tree(const UIElement* el, int* k, const UIElement* asking, int* asked) {
    if (el->kind == UI_EDITOR) {
        if (el == asking)
            *asked = *k;
        printf("%d", (*k)++);
        return;
    }
    printf("(");
    for (size_t i = 0; i < el->child_count; i++) {
        printf(i ? "," : "");
        say_tree(el->children[i], k, asking, asked);
    }
    printf(")");
}

// The UI's input pass with the real pointer and keyboard, as the engine read them this frame.
static void live_ui_update(Engine* engine) {
    if (!g_ui_live)
        return;
    UIInput in = {0};
    ui_input_read(&in, engine);
    ui_update(g_ui_live, &in, (float)engine->win_width, (float)engine->win_height);
}

// What the UI is handed to type this frame, through the helper an app uses, one token an event:
// `c<hex>` a character, `k<key>+<mods>` a key.
static void say_typed(const Engine* engine) {
    UIInput in = {0};
    ui_input_read(&in, engine);
    if (in.key_event_count == 0)
        return;
    printf("canvas typed");
    for (int i = 0; i < in.key_event_count; i++) {
        const InputKeyEvent* ev = &in.key_events[i];
        if (ev->key)
            printf(" k%d+%d", ev->key, ev->mods);
        else
            printf(" c%x", (unsigned)ev->codepoint);
    }
    printf("\n");
}

static void on_idle_loaded(Texture* tex, void* user) {
    (void)user;
    if (g_idle_say)
        printf("canvas idle loaded %d %d\n", tex ? tex->width : 0, tex ? tex->height : 0);
}

// The work the board's thread and --idle-load stand for, started on the first frame with no timed
// wake left to ask for. After a wake the run is past the first tenth of a second, in which a hidden
// window is sent something that wakes a waiting loop, so nothing but the work can wake it.
static void start_late_work(Engine* engine) {
    if (g_late_started || *g_wake_spec)
        return;
    g_late_started = true;
    if (g_late_edit) {
        board_late_start(engine);
        if (g_idle_say)
            printf("canvas idle work begun %zu\n", engine->total_frames + 1);
    }
    if (g_idle_load)
        engine_load_texture(engine, engine_get_scene(engine)->tex_pool, g_idle_load,
                            texture_desc(true), on_idle_loaded, NULL);
}

static void history_stream(Engine* engine);

static void on_update(Engine* engine, float dt) {
    (void)dt;
    if (board_late_settle(false) && g_idle_say)
        printf("canvas idle work landed %zu\n", engine->total_frames + 1);
    if (g_idle_say) {
        printf("canvas idle frame %zu delta %.6f\n", engine->total_frames + 1, engine->delta_time);
        printf("canvas idle at %zu %.6f\n", engine->total_frames + 1, glfwGetTime());
    }
    start_late_work(engine);
    for (char sep = '+'; *g_wake_spec && sep == '+';) {
        char* end;
        const double seconds = strtod(g_wake_spec, &end);
        if (end == g_wake_spec) {
            g_wake_spec = "";
            break;
        }
        engine_request_frame_after(engine, seconds);
        sep = *end;
        g_wake_spec = sep ? end + 1 : end;
    }
    if (g_probe)
        say_typed(engine);
    live_ui_update(engine);
    if (g_editor_sheet.file_count)
        editor_host_frame(engine);
    history_stream(engine);
    // The input pass an app owes its UI, with no input: it is what settles the focused button's
    // state, which its ease then follows.
    if (g_ui_enter)
        ui_update(g_ui_enter, &(UIInput){0}, (float)engine->win_width, (float)engine->win_height);
    // On the first frame, after it has begun: what a later frame does with it is idle mode's to
    // arrange.
    if (engine->total_frames == 0 && g_gui_click) {
        // Queued as GLFW's callbacks queue them, all three in one batch: ImGui takes them a frame
        // at a time.
        ImGuiIO* io = igGetIO_Nil();
        ImGuiIO_AddMousePosEvent(io, (float)engine->win_width * 0.5f,
                                 (float)engine->win_height * 0.5f);
        ImGuiIO_AddMouseButtonEvent(io, 0, true);
        ImGuiIO_AddMouseButtonEvent(io, 0, false);
    }
}

static void on_cursor(Engine* engine, double fb_x, double fb_y) {
    double px, py;
    if (!engine_fb_to_points(engine, fb_x, fb_y, &px, &py))
        return;
    const float x = (float)px, y = (float)py;
    if (g_drag_view >= 0)
        canvas_view_pan(&g_views[g_drag_view], x - g_last_x, y - g_last_y);
    g_last_x = x, g_last_y = y;
}

// Whether the pointer is over the live UI, which then takes the press or the wheel the canvas
// would have.
static bool over_ui(void) {
    return g_ui_live && ui_pointer_over(g_ui_live, g_last_x, g_last_y);
}

static void on_button(Engine* engine, int button, int action, int mods) {
    (void)engine;
    (void)mods;
    if (button != GLFW_MOUSE_BUTTON_LEFT)
        return;
    g_drag_view = (action == GLFW_PRESS && !over_ui()) ? view_at(g_last_x) : -1;
}

static void on_scroll(Engine* engine, double xoffset, double yoffset) {
    (void)engine;
    (void)xoffset;
    if (!over_ui())
        canvas_view_zoom_at(&g_views[view_at(g_last_x)], g_last_x, g_last_y, pow(1.15, yoffset));
}

/*
 * --history: a column at the window's right with a history in a scrolling pane, the shape a chat
 * takes -- wrapped messages, some with styled runs, at the end of a pane that follows its end. It
 * tests the engine's pane and text; what a message says is made up, and is no sketch of pcb's.
 */
#define HISTORY_WIDTH    360.0f
#define HISTORY_MESSAGES 200

static const char* HISTORY_LINES[] = {
    "moved C12 next to U3 pin 7",
    "routed SDA from U1 to J2 in three segments, keeping clear of the crystal",
    "the check found two problems near the USB connector: a clearance of 0.09 mm and an unrouted "
    "pad on J1",
    "placed R17 and R18 beside the regulator",
    "why does the ground pour stop short of the mounting hole?",
    "widened VBUS to 0.5 mm along its whole length",
    "ok",
    "flipped U4 to the bottom side and moved its decoupling with it",
};

// A reply streamed into the history a word a frame, as an agent's answer arrives.
static const char* HISTORY_REPLY =
    "checking the board for that now: the ground pour keeps 0.3 mm from every pad and the "
    "mounting hole has its own clearance, so the pour stops where the clearance does";

#define HISTORY_MAX 4096 // messages kept; past it the oldest is not dropped, the newest refused

static UIScreen* g_history; // the column's screen
static UIElement* g_history_pane;
static UIElement* g_history_input;
static Font* g_mono;     // the second face, for a part's name
static Engine* g_engine; // for the input's submit, which has no engine of its own
// The history, the app's own, which the pane builds rows from.
typedef struct Message {
    char* text; // "speaker: what was said"
    bool agent; // the agent said it, rather than you
    bool mono;  // a part it names is set in the second face
} Message;
static Message g_msgs[HISTORY_MAX];
static int g_msg_count;
static int g_reply = -1;  // the message being streamed into, or -1
static size_t g_reply_at; // how much of HISTORY_REPLY is in it

// A message at the end of the history, and a row for it; its index, or -1 when the history is full.
static int history_push(bool agent, const char* said, bool mono) {
    if (g_msg_count >= HISTORY_MAX)
        return -1;
    char text[512];
    snprintf(text, sizeof(text), "%s: %s", agent ? "agent" : "you", said);
    g_msgs[g_msg_count] = (Message){.text = strdup(text), .agent = agent, .mono = mono};
    if (g_history_pane && g_history_pane->rows)
        ui_scroll_set_row_count(g_history_pane, g_msg_count + 1);
    return g_msg_count++;
}

/*
 * A message's styled runs from byte `from` of its text on, counted from `from`: the speaker in a
 * colour, and a part it names in the second face. The same for a row built whole and for a reply
 * appended to as it streams, so the two look alike.
 */
static size_t message_runs(const Message* m, size_t from, UITextRun runs[2]) {
    size_t count = 0;
    if (from == 0) {
        const bool agent = m->agent;
        runs[count++] = (UITextRun){
            .start = 0,
            .length = strcspn(m->text, ":"),
            .color = {agent ? 0.45f : 0.55f, agent ? 0.70f : 0.85f, agent ? 1.0f : 0.55f, 1.0f}};
    }
    const char* part = strstr(m->text + from, "C12");
    if (g_mono && m->mono && part)
        runs[count++] =
            (UITextRun){.start = (size_t)(part - m->text) - from, .length = 3, .font = g_mono};
    return count;
}

/*
 * A message's row: a read-only field, so its text can be selected and copied, wrapped across the
 * column and as tall as its text, in the column's colour with no border.
 */
static void build_message(UIElement* row, int index, void* user) {
    (void)user;
    UIElement* f = ui_text_field(row, UI_FIELD_MULTILINE | UI_FIELD_READ_ONLY, NULL, NULL);
    ui_set_size(f, UI_GROW, 0.0f, UI_FIT, 0.0f);
    ui_set_style(f, &(UIStyle){.bg = {0.10f, 0.11f, 0.14f, 1.0f},
                               .border = {0.10f, 0.11f, 0.14f, 1.0f},
                               .padding = {1.0f, 1.0f, 1.0f, 1.0f}});
    const Message* m = &g_msgs[index];
    ui_field_set_text(f, m->text);
    UITextRun runs[2];
    ui_set_text_runs(f, runs, message_runs(m, 0, runs));
}

// The input line's Enter: the message joins the history and a reply starts to stream.
static void on_history_submit(UIElement* el, void* user) {
    (void)user;
    if (!ui_field_text(el)[0])
        return;
    history_push(false, ui_field_text(el), true);
    ui_field_set_text(el, "");
    g_reply = history_push(true, "", true);
    g_reply_at = 0;
    if (g_engine)
        engine_request_frame(g_engine);
}

// The next word of the reply, a frame at a time headless and every 50 ms in a window, asking for
// the frame it will be drawn in. A built row has the word appended, keeping whatever is selected
// in it; a row built later is built from the message as it then is.
static void history_stream(Engine* engine) {
    if (g_reply < 0)
        return;
    static double next;
    const double now = glfwGetTime();
    if (!engine->headless && now < next) {
        engine_request_frame_after(engine, next - now);
        return;
    }
    const char* rest = HISTORY_REPLY + g_reply_at;
    const char* space = strchr(rest + (*rest == ' '), ' ');
    g_reply_at = space ? (size_t)(space - HISTORY_REPLY) : strlen(HISTORY_REPLY);
    Message* m = &g_msgs[g_reply];
    const size_t had = strlen(m->text);
    char text[520];
    snprintf(text, sizeof(text), "agent: %.*s", (int)g_reply_at, HISTORY_REPLY);
    free(m->text);
    m->text = strdup(text);
    UIElement* row = ui_scroll_find_row(g_history_pane, g_reply);
    if (row && row->child_count) {
        UITextRun runs[2];
        ui_field_append(row->children[0], m->text + had, runs, message_runs(m, had, runs));
    }
    if (!space) {
        g_reply = -1;
        return;
    }
    next = now + 0.05;
    if (engine->headless)
        engine_request_frame(engine);
    else
        engine_request_frame_after(engine, 0.05);
}

static void build_history(UISystem* ui, Font* mono) {
    g_mono = mono;
    UIScreen* s = ui_screen(ui, "history");
    g_history = s;
    ui_screen_set_modal(s, false);
    UIElement* root = ui_screen_root(s);
    root->dir = UI_ROW;
    root->align_main = UI_ALIGN_END;
    UIElement* column = ui_panel(root);
    ui_set_size(column, UI_FIXED, HISTORY_WIDTH, UI_GROW, 0.0f);
    ui_set_style(column, &(UIStyle){.bg = {0.10f, 0.11f, 0.14f, 1.0f},
                                    .corner_radius = 1.0f,
                                    .padding = {12.0f, 12.0f, 12.0f, 12.0f}});
    column->spacing = 10.0f;
    const size_t kinds = sizeof(HISTORY_LINES) / sizeof(HISTORY_LINES[0]);
    for (int i = 0; i < HISTORY_MESSAGES; i++) {
        char said[256];
        snprintf(said, sizeof(said), "%s (%d)", HISTORY_LINES[(size_t)i % kinds], i);
        // Every fifth line naming C12 shows it in the second face.
        history_push(i % 3 != 1, said, i % 5 == 0);
    }
    UIElement* pane = ui_scroll(column);
    g_history_pane = pane;
    ui_set_size(pane, UI_GROW, 0.0f, UI_GROW, 0.0f);
    pane->follow_end = true;
    pane->spacing = 10.0f;
    ui_scroll_set_rows(pane, g_msg_count, 40.0f, build_message, NULL);
    g_history_input = ui_text_field(column, UI_FIELD_MULTILINE | UI_FIELD_SUBMIT_ON_ENTER,
                                    on_history_submit, NULL);
    ui_field_set_placeholder(g_history_input, "Message");
    ui_set_size(g_history_input, UI_GROW, 0.0f, UI_FIT, 0.0f);
    g_history_input->max_rows = 6;
    ui_push(ui, s);
}

/*
 * --palette: Cmd+K (Ctrl+K elsewhere) opens a field over the app's own commands, each with its key,
 * filtered and ordered by fuzzy_match with the matched letters highlighted; `/` opens a jump over
 * generated names the same way. Up and Down move the highlight, Enter or a click runs it, Esc
 * closes. Matching is fuzzy_match's and the tie-break the list's own order, as pcb's will be its
 * natural order.
 */
#define PALETTE_ROWS 12
#define JUMP_NAMES   5000

typedef struct Command {
    const char* name;
    const char* key;
    void (*run)(Engine* engine);
} Command;

static CanvasView g_home_view; // the first view as it began, which Reset view goes back to

static void cmd_fps(Engine* e) {
    e->show_fps = !e->show_fps;
}
static void cmd_gui(Engine* e) {
    e->show_gui = !e->show_gui;
}
static void cmd_zoom(Engine* e, double by) {
    CanvasView* v = &g_views[0];
    (void)e;
    canvas_view_zoom_at(v, v->x + v->width * 0.5f, v->y + v->height * 0.5f, by);
}
static void cmd_zoom_in(Engine* e) {
    cmd_zoom(e, 1.25);
}
static void cmd_zoom_out(Engine* e) {
    cmd_zoom(e, 0.8);
}
static void cmd_left(Engine* e) {
    (void)e;
    canvas_view_pan(&g_views[0], 60.0f, 0.0f);
}
static void cmd_right(Engine* e) {
    (void)e;
    canvas_view_pan(&g_views[0], -60.0f, 0.0f);
}
static void cmd_up(Engine* e) {
    (void)e;
    canvas_view_pan(&g_views[0], 0.0f, 60.0f);
}
static void cmd_down(Engine* e) {
    (void)e;
    canvas_view_pan(&g_views[0], 0.0f, -60.0f);
}
static void cmd_reset(Engine* e) {
    (void)e;
    const CanvasView keep = g_views[0];
    g_views[0] = g_home_view;
    g_views[0].x = keep.x, g_views[0].y = keep.y;
    g_views[0].width = keep.width, g_views[0].height = keep.height;
}
static void cmd_print(Engine* e) {
    (void)e;
    printf("canvas view centre %lld %lld\n", (long long)g_views[0].centre.x,
           (long long)g_views[0].centre.y);
}

static const Command COMMANDS[] = {
    {"Toggle FPS readout", "F", cmd_fps},
    {"Toggle debug GUI", "G", cmd_gui},
    {"Zoom in", "=", cmd_zoom_in},
    {"Zoom out", "-", cmd_zoom_out},
    {"Pan left", "", cmd_left},
    {"Pan right", "", cmd_right},
    {"Pan up", "", cmd_up},
    {"Pan down", "", cmd_down},
    {"Reset view", "0", cmd_reset},
    {"Print view centre", "P", cmd_print},
};
#define COMMAND_COUNT ((int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])))

#define PALETTE_ROW_HEIGHT 30.0f

typedef struct Hit {
    int index, score;
} Hit;

static UIScreen* g_palette;
static UIElement* g_pal_field;
static UIElement* g_pal_list;      // the pane of rows, one a hit
static Hit g_pal_hits[JUMP_NAMES]; // what the field matches, best first
static int g_pal_count, g_pal_selected;
static bool g_pal_jump; // the list is the jump's names rather than the commands
static char g_jump_names[JUMP_NAMES][24];

static const char* pal_name(int i) {
    return g_pal_jump ? g_jump_names[i] : COMMANDS[i].name;
}

static int hit_order(const void* a, const void* b) {
    const Hit* x = a;
    const Hit* y = b;
    if (x->score != y->score)
        return y->score - x->score;
    return x->index - y->index;
}

// The runs lighting what the field matched in `name`, one a matched character; how many.
static size_t palette_lit(const char* name, UITextRun* lit) {
    FuzzyPositions matched;
    if (fuzzy_match(ui_field_text(g_pal_field), name, &matched) < 0)
        return 0;
    for (size_t n = 0; n < matched.count; n++) {
        const char* end = name + matched.at[n];
        text_utf8_next(&end);
        lit[n] = (UITextRun){.start = matched.at[n],
                             .length = (size_t)(end - (name + matched.at[n])),
                             .color = {1.0f, 0.78f, 0.30f, 1.0f}};
    }
    return matched.count;
}

static void on_palette_row(UIElement* el, void* user);

/*
 * Hit `index`'s row: its name with the matched letters lit, its key at the right, on the focus
 * colour when it is the one Enter runs. A click runs it.
 */
static void build_palette_row(UIElement* holder, int index, void* user) {
    (void)user;
    const int item = g_pal_hits[index].index;
    UIElement* row = ui_panel(holder);
    row->dir = UI_ROW;
    row->focusable = true;
    row->action = on_palette_row;
    row->action_user = (void*)(intptr_t)index;
    ui_set_size(row, UI_GROW, 0.0f, UI_FIXED, PALETTE_ROW_HEIGHT);
    const bool lit = index == g_pal_selected;
    ui_set_style(row, &(UIStyle){.bg = {0.16f, 0.34f, 0.62f, lit ? 1.0f : 0.0f},
                                 .corner_radius = 5.0f,
                                 .padding = {5.0f, 10.0f, 5.0f, 10.0f}});
    UIElement* name = ui_label(row, pal_name(item));
    ui_set_size(name, UI_GROW, 0.0f, UI_FIT, 0.0f);
    UITextRun runs[FUZZY_QUERY_MAX];
    ui_set_text_runs(name, runs, palette_lit(pal_name(item), runs));
    UIElement* key = ui_label(row, g_pal_jump ? "" : COMMANDS[item].key);
    // A label's own room above and below its line would not fit the row's fixed height.
    name->padding[0] = name->padding[2] = key->padding[0] = key->padding[2] = -1.0f;
}

/*
 * The list for what is in the field: every item that matches, best first and in the list's own
 * order among equals, a row each.
 */
static void palette_filter(void) {
    const char* query = ui_field_text(g_pal_field);
    const int total = g_pal_jump ? JUMP_NAMES : COMMAND_COUNT;
    int found = 0;
    for (int i = 0; i < total; i++) {
        const int score = fuzzy_match(query, pal_name(i), NULL);
        if (score >= 0)
            g_pal_hits[found++] = (Hit){i, score};
    }
    qsort(g_pal_hits, (size_t)found, sizeof(Hit), hit_order);
    g_pal_count = found;
    g_pal_selected = 0;
    ui_scroll_set_rows(g_pal_list, found, PALETTE_ROW_HEIGHT, build_palette_row, NULL);
}

static void palette_close(void) {
    UISystem* ui = g_ui_live;
    if (ui_top(ui) == g_palette)
        ui_pop(ui);
}

// Runs the item at row `r` and closes the palette; a name from the jump is said, there being
// nothing in this app to jump to.
static void palette_run(int r) {
    if (r < 0 || r >= g_pal_count)
        return;
    const int item = g_pal_hits[r].index;
    palette_close();
    if (g_pal_jump) {
        printf("canvas jump %s\n", g_jump_names[item]);
        return;
    }
    if (g_probe)
        printf("canvas palette ran %d\n", item);
    COMMANDS[item].run(g_engine);
}

static void on_palette_change(UIElement* el, void* user) {
    (void)el;
    (void)user;
    palette_filter();
}

static void on_palette_submit(UIElement* el, void* user) {
    (void)el;
    (void)user;
    palette_run(g_pal_selected);
}

static void on_palette_row(UIElement* el, void* user) {
    (void)el;
    palette_run((int)(intptr_t)user);
}

// Up and Down move the highlight while the caret stays in the field, the list scrolled to show it.
static bool on_palette_key(UIElement* el, const InputKeyEvent* key, void* user) {
    (void)el;
    (void)user;
    if (key->key != GLFW_KEY_UP && key->key != GLFW_KEY_DOWN)
        return false;
    const int step = key->key == GLFW_KEY_UP ? -1 : 1;
    const int to = g_pal_selected + step;
    if (to >= 0 && to < g_pal_count) {
        ui_scroll_rows_changed(g_pal_list, g_pal_selected, 1);
        ui_scroll_rows_changed(g_pal_list, to, 1);
        g_pal_selected = to;
        ui_scroll_to_row(g_ui_live, g_pal_list, to);
    }
    return true;
}

static void build_palette(UISystem* ui) {
    for (int i = 0; i < JUMP_NAMES; i++) {
        static const char* kinds[] = {"net", "U", "R", "C", "bank/mem"};
        const int k = i % 5;
        if (k == 0)
            snprintf(g_jump_names[i], sizeof(g_jump_names[i]), "net_%04d", i / 5);
        else if (k == 4)
            snprintf(g_jump_names[i], sizeof(g_jump_names[i]), "bank/mem[%d]", i / 5);
        else
            snprintf(g_jump_names[i], sizeof(g_jump_names[i]), "%s%d", kinds[k], i / 5 + 1);
    }
    g_palette = ui_screen(ui, "palette");
    UIElement* box = ui_panel(ui_screen_root(g_palette));
    ui_set_size(box, UI_FIXED, 420.0f, UI_FIT, 0.0f);
    box->spacing = 6.0f;
    g_pal_field = ui_text_field(box, 0, on_palette_submit, NULL);
    ui_set_size(g_pal_field, UI_GROW, 0.0f, UI_FIT, 0.0f);
    ui_field_set_on_change(g_pal_field, on_palette_change, NULL);
    ui_set_on_key(g_pal_field, on_palette_key, NULL);
    // The rows in a pane with no gaps between them, as tall as they are up to PALETTE_ROWS of them,
    // and scrolling past that.
    g_pal_list = ui_scroll(box);
    ui_set_size(g_pal_list, UI_GROW, 0.0f, UI_FIT, 0.0f);
    g_pal_list->max_size[1] = PALETTE_ROWS * PALETTE_ROW_HEIGHT;
    g_pal_list->spacing = -1.0f;
}

static void palette_open(bool jump) {
    g_pal_jump = jump;
    ui_field_set_placeholder(g_pal_field, jump ? "Jump to a part or net" : "Type a command");
    ui_field_set_text(g_pal_field, "");
    palette_filter();
    if (!ui_on_stack(g_ui_live, g_palette))
        ui_push(g_ui_live, g_palette);
    ui_set_focus(g_palette, g_pal_field);
}

// What the canvas app's own keys do, in a view: G the debug GUI, F the FPS readout, = and - zoom, 0
// resets the view, P prints it; Enter puts the caret in the history's input line, Cmd+K (Ctrl+K)
// opens the palette and / the jump. None of them while the UI takes the keyboard.
static void on_key(Engine* engine, int key, int scancode, int action, int mods) {
    (void)scancode;
    if (action != GLFW_PRESS || (g_ui_live && ui_wants_keyboard(g_ui_live)))
        return;
    const bool shortcut = (mods & INPUT_MOD_SHORTCUT) != 0;
    if (g_palette && key == GLFW_KEY_K && shortcut)
        palette_open(false);
    else if (g_palette && key == GLFW_KEY_SLASH && !mods)
        palette_open(true);
    else if (g_history && (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER))
        ui_set_focus(g_history, g_history_input);
    else if (key == GLFW_KEY_G)
        cmd_gui(engine);
    else if (key == GLFW_KEY_F)
        cmd_fps(engine);
    else if (key == GLFW_KEY_EQUAL)
        cmd_zoom_in(engine);
    else if (key == GLFW_KEY_MINUS)
        cmd_zoom_out(engine);
    else if (key == GLFW_KEY_0)
        cmd_reset(engine);
    else if (key == GLFW_KEY_P)
        cmd_print(engine);
}

// A rect in window points as its corners in framebuffer pixels, `s` to a point, after a space: the
// box a gate crops the screenshot by.
static void say_box(UIRect r, double s) {
    printf(" %ld %ld %ld %ld", lround(r.x * s), lround(r.y * s), lround((r.x + r.w) * s),
           lround((r.y + r.h) * s));
}

/*
 * --ui-borders (spec 14.12): boxes of one size and radius on one opaque backdrop, which
 * canvas-ui-borders holds to Porter-Duff over. A border over a clear fill must draw as one over a
 * fill of the backdrop's own colour, and one over a fill at half alpha as one over that fill laid
 * on the backdrop. The clear fill is magenta at alpha 0, a colour that must count for nothing: an
 * all-zero colour would mean "inherit". And an opaque border's outer edge must draw as the edge of
 * a box filled with the border's colour, which it does only if the edge's coverage counts once.
 */
static void build_borders(UISystem* ui, int width, int height, double scale, bool probe) {
#define BORDERS_BACKDROP 0.16f, 0.20f, 0.26f
#define BORDERS_FILL     0.80f, 0.30f, 0.20f
#define BORDERS_BLUE     0.45f, 0.70f, 1.00f
    static const struct {
        const char* name;
        UIStyle style;
    } BOXES[] = {
        {"opaque", {.bg = {BORDERS_BACKDROP, 1.0f}, .border = {BORDERS_BLUE, 0.6f}}},
        {"clear", {.bg = {1.0f, 0.0f, 1.0f, 0.0f}, .border = {BORDERS_BLUE, 0.6f}}},
        {"half", {.bg = {BORDERS_FILL, 0.5f}, .border = {BORDERS_BLUE, 0.6f}}},
        {"mixed",
         {.bg = {0.5f * (0.80f + 0.16f), 0.5f * (0.30f + 0.20f), 0.5f * (0.20f + 0.26f), 1.0f},
          .border = {BORDERS_BLUE, 0.6f}}},
        {"edge", {.bg = {BORDERS_FILL, 1.0f}, .border = {BORDERS_BLUE, 1.0f}}},
        {"solid", {.bg = {BORDERS_BLUE, 1.0f}}},
    };
    const size_t count = sizeof(BOXES) / sizeof(BOXES[0]);
    UIScreen* screen = ui_screen(ui, "borders");
    UIElement* backdrop = ui_panel(ui_screen_root(screen));
    backdrop->dir = UI_ROW;
    ui_set_style(backdrop, &(UIStyle){.bg = {BORDERS_BACKDROP, 1.0f}});
    UIElement* made[sizeof(BOXES) / sizeof(BOXES[0])];
    for (size_t i = 0; i < count; i++) {
        UIStyle style = BOXES[i].style;
        style.border_width = 2.0f;
        style.corner_radius = 8.0f;
        made[i] = ui_panel(backdrop);
        ui_set_size(made[i], UI_FIXED, 50.0f, UI_FIXED, 40.0f);
        ui_set_style(made[i], &style);
    }
#undef BORDERS_BACKDROP
#undef BORDERS_FILL
#undef BORDERS_BLUE
    ui_push(ui, screen);
    ui_layout(screen, (float)width, (float)height);
    for (size_t i = 0; probe && i < count; i++) {
        printf("canvas border %s", BOXES[i].name);
        say_box(made[i]->rect, scale);
        printf("\n");
    }
}

/*
 * --ui-seams (spec 14.23): boxes of one colour on an opaque backdrop of another, which
 * canvas-ui-seams holds to a one-pixel edge. A column of quads whole pixels high meeting on pixel
 * boundaries, as pcb's cross-section's rows, with nothing between them; a quad on whole pixels,
 * filled whole and nothing past it; a quad whose right edge falls half way across a column of
 * pixels, those pixels half way; a box with rounded corners; and the column again with its rows
 * meeting part way into pixels, where each of two quads covers the pixel in part and no more than
 * a quarter of the backdrop shows. Drawn with ui_draw_quad and ui_draw_rounded, as pcb draws its
 * rows, since a style's corner radius of 0 is the theme's. Its lengths are framebuffer pixels,
 * from the window's corner.
 */
static const vec4 SEAMS_BACKDROP = {0.16f, 0.20f, 0.26f, 1.0f},
                  SEAMS_BOX = {0.80f, 0.30f, 0.20f, 1.0f};
static const float SEAMS_RADIUS = 12.0f;
static const float SEAMS_ROWS[] = {7.0f, 5.0f, 9.0f, 6.0f, 11.0f, 4.0f, 8.0f};
#define SEAMS_ROW_COUNT (sizeof(SEAMS_ROWS) / sizeof(SEAMS_ROWS[0]))
typedef enum { SEAMS_QUAD, SEAMS_ROUNDED, SEAMS_COLUMN } SeamsKind;
static const struct {
    const char* name;
    SeamsKind kind;
    float x, y, w;
    float h;         // a quad's; a column is as high as its rows
    float taller[2]; // a column's even and odd rows, each this much taller than SEAMS_ROWS says
} SEAMS[] = {{"stack", SEAMS_COLUMN, 20.0f, 20.0f, 40.0f},
             {"alone", SEAMS_QUAD, 80.0f, 20.0f, 40.0f, 30.0f},
             {"round", SEAMS_ROUNDED, 140.0f, 20.0f, 40.0f, 40.0f},
             {"half", SEAMS_QUAD, 200.0f, 20.0f, 30.5f, 30.0f},
             {"offgrid", SEAMS_COLUMN, 260.0f, 20.0f, 40.0f, .taller = {0.5f, 0.25f}}};
#define SEAMS_COUNT (sizeof(SEAMS) / sizeof(SEAMS[0]))

// Box `i` in pixels, from the window's corner.
static UIRect seams_box(size_t i) {
    float h = SEAMS[i].h;
    if (SEAMS[i].kind == SEAMS_COLUMN) {
        for (size_t k = 0; k < SEAMS_ROW_COUNT; k++)
            h += SEAMS_ROWS[k] + SEAMS[i].taller[k % 2];
    }
    return (UIRect){SEAMS[i].x, SEAMS[i].y, SEAMS[i].w, h};
}

// A colour as 0xRRGGBB, as the gate's probe lines name one.
static uint32_t seams_rgb(const float c[4]) {
    return (uint32_t)lroundf(c[0] * 255.0f) << 16 | (uint32_t)lroundf(c[1] * 255.0f) << 8 |
           (uint32_t)lroundf(c[2] * 255.0f);
}

static void draw_seams(UIElement* el, UIDrawList* dl, void* user) {
    const float px = *(const float*)user; // points a pixel
    ui_draw_quad(dl, el->rect, (float*)SEAMS_BACKDROP);
    for (size_t i = 0; i < SEAMS_COUNT; i++) {
        const UIRect b = seams_box(i), r = {b.x * px, b.y * px, b.w * px, b.h * px};
        if (SEAMS[i].kind == SEAMS_QUAD) {
            ui_draw_quad(dl, r, (float*)SEAMS_BOX);
        } else if (SEAMS[i].kind == SEAMS_ROUNDED) {
            ui_draw_rounded(dl, r, SEAMS_RADIUS * px, (float*)SEAMS_BOX, (vec4){0}, 0.0f);
        } else {
            float y = b.y;
            for (size_t k = 0; k < SEAMS_ROW_COUNT; k++) {
                const float h = SEAMS_ROWS[k] + SEAMS[i].taller[k % 2];
                ui_draw_quad(dl, (UIRect){r.x, y * px, r.w, h * px}, (float*)SEAMS_BOX);
                y += h;
            }
        }
    }
}

static void build_seams(UISystem* ui, double scale, bool probe) {
    static float px;
    px = (float)(1.0 / scale);
    UIScreen* screen = ui_screen(ui, "seams");
    UIElement* panel = ui_panel(ui_screen_root(screen));
    ui_set_size(panel, UI_GROW, 0.0f, UI_GROW, 0.0f);
    ui_set_draw(panel, draw_seams, &px);
    ui_push(ui, screen);
    if (!probe)
        return;
    printf("canvas seams colours want %06x backdrop %06x\n", seams_rgb(SEAMS_BOX),
           seams_rgb(SEAMS_BACKDROP));
    // Each box rounded outward, so the half quad's last column is the one its edge halves.
    for (size_t i = 0; i < SEAMS_COUNT; i++) {
        const UIRect b = seams_box(i);
        printf("canvas seams %s %.0f %.0f %.0f %.0f\n", SEAMS[i].name, (double)b.x, (double)b.y,
               ceil((double)(b.x + b.w)), ceil((double)(b.y + b.h)));
    }
}

static void usage(const char* argv0) {
    fprintf(
        stderr,
        "usage: %s [-x] [-f frames] [-S out.ppm] [-W points] [-H points]\n"
        "          [--sheet NAME] [--view X,Y,WIDTH (mm)] [--offset UNITS] [--mirror]\n"
        "          [--tile-shift N] [--canvas-probe] [--picture] [--edit-thread] [--work-delay "
        "MS]\n"
        "          [--split] [--only-view 0|1] [--ui] [--budget]\n"
        "          [--idle] [--idle-wake S,...] [--idle-load PATH] [--input-script PATH]\n"
        "          [--ui-enter] [--ui-text TEXT] [--ui-keys] [--gui-click] [--history] "
        "[--palette]\n"
        "          [--font-sheet] [--editor FILE] [--editor-lag FRAMES] [--vim] [--read-only]\n"
        "          [--editor-fold FIRST,LAST] [--editor-filler LINE,ROWS] [--refuse-close]\n"
        "          [--ui-borders] [--ui-seams] [--dash LENGTH,GAP (mm)] [--dash-min-px PX]\n"
        "          [--cap round|butt|square] [--join round|mitre|bevel] [--dim STRENGTH,ROOM]\n",
        argv0);
}

// An after-tonemap pass that changes nothing, so the frame is drawn into the
// picture rather than the window, as a CRT or a grade puts it.
static const char* IDENTITY_PASS = "#version 330 core\n"
                                   "in vec2 TexCoords;\n"
                                   "out vec4 FragColor;\n"
                                   "uniform sampler2D sceneColor;\n"
                                   "void main() {\n"
                                   "    FragColor = vec4(texelFetch(sceneColor, "
                                   "ivec2(gl_FragCoord.xy), 0).rgb, 1.0);\n"
                                   "}\n";

int main(int argc, char** argv) {
    bool headless = false, mirror = false, probe = false, picture = false, edit_thread = false;
    bool split = false, with_ui = false, budget = false, idle = false, ui_enter = false,
         borders = false, seams = false;
    bool history = false, palette = false, font_sheet = false, vim = false;
    const char* editor_files[EDITOR_FILES_MAX];
    int editor_count = 0;
    int frames = 0, width = 960, height = 600, tile_shift = 0, only_view = -1;
    const char* screenshot = NULL;
    const char* input_script = NULL;
    const char* sheet_name = "shapes";
    double view_x = NAN, view_y = NAN, view_w = NAN;
    float dash_min_px = NAN;
    long long offset = 0;
    for (int i = 1; i < argc; i++) {
        const bool more = i + 1 < argc;
        if (strcmp(argv[i], "-x") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "-f") == 0 && more) {
            frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-S") == 0 && more) {
            screenshot = argv[++i];
        } else if (strcmp(argv[i], "-W") == 0 && more) {
            width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-H") == 0 && more) {
            height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--sheet") == 0 && more) {
            sheet_name = argv[++i];
        } else if (strcmp(argv[i], "--view") == 0 && more) {
            if (sscanf(argv[++i], "%lf,%lf,%lf", &view_x, &view_y, &view_w) != 3) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--offset") == 0 && more) {
            offset = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--mirror") == 0) {
            mirror = true;
        } else if (strcmp(argv[i], "--tile-shift") == 0 && more) {
            tile_shift = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--canvas-probe") == 0) {
            probe = true;
        } else if (strcmp(argv[i], "--picture") == 0) {
            picture = true;
        } else if (strcmp(argv[i], "--edit-thread") == 0) {
            edit_thread = true;
        } else if (strcmp(argv[i], "--work-delay") == 0 && more) {
            const int ms = atoi(argv[++i]);
            g_work_delay_ms = ms > 0 ? (unsigned)ms : 0u;
        } else if (strcmp(argv[i], "--split") == 0) {
            split = true;
        } else if (strcmp(argv[i], "--only-view") == 0 && more) {
            only_view = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--ui") == 0) {
            with_ui = true;
        } else if (strcmp(argv[i], "--budget") == 0) {
            budget = true;
        } else if (strcmp(argv[i], "--idle") == 0) {
            idle = true;
        } else if (strcmp(argv[i], "--idle-wake") == 0 && more) {
            // Seconds, comma-separated: each item asked for on its frame in turn, and the times
            // of an item joined by + on the same frame.
            g_wake_spec = argv[++i];
        } else if (strcmp(argv[i], "--input-script") == 0 && more) {
            input_script = argv[++i];
        } else if (strcmp(argv[i], "--idle-load") == 0 && more) {
            g_idle_load = argv[++i];
        } else if (strcmp(argv[i], "--ui-enter") == 0) {
            with_ui = true;
            ui_enter = true;
        } else if (strcmp(argv[i], "--history") == 0) {
            history = true;
        } else if (strcmp(argv[i], "--font-sheet") == 0) {
            font_sheet = true;
        } else if (strcmp(argv[i], "--editor") == 0 && more) {
            if (editor_count == EDITOR_FILES_MAX) {
                fprintf(stderr, "canvas: --editor %d files at the most\n", EDITOR_FILES_MAX);
                return 1;
            }
            editor_files[editor_count++] = argv[++i];
        } else if (strcmp(argv[i], "--editor-lag") == 0 && more) {
            g_editor_sheet.lag = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--editor-fold") == 0 && more) {
            if (sscanf(argv[++i], "%d,%d", &g_editor_sheet.fold_first, &g_editor_sheet.fold_last) !=
                2) {
                fprintf(stderr, "canvas: --editor-fold takes FIRST,LAST\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--editor-filler") == 0 && more) {
            if (sscanf(argv[++i], "%d,%d", &g_editor_sheet.filler_line,
                       &g_editor_sheet.filler_rows) != 2) {
                fprintf(stderr, "canvas: --editor-filler takes LINE,ROWS\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--split-share") == 0 && more) {
            g_split_share = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--vim") == 0) {
            vim = true;
        } else if (strcmp(argv[i], "--vim-lookup") == 0) {
            g_editor_sheet.lookup = true;
        } else if (strcmp(argv[i], "--vim-layout") == 0) {
            g_editor_sheet.layout = true;
        } else if (strcmp(argv[i], "--read-only") == 0) {
            g_editor_sheet.read_only = true;
        } else if (strcmp(argv[i], "--refuse-close") == 0) {
            g_refuse_close = true;
        } else if (strcmp(argv[i], "--palette") == 0) {
            palette = true;
        } else if (strcmp(argv[i], "--ui-text") == 0 && more) {
            with_ui = true;
            g_ui_text = argv[++i];
        } else if (strcmp(argv[i], "--ui-keys") == 0) {
            with_ui = true;
            g_ui_keys = true;
        } else if (strcmp(argv[i], "--gui-click") == 0) {
            g_gui_click = true;
        } else if (strcmp(argv[i], "--ui-borders") == 0) {
            borders = true;
        } else if (strcmp(argv[i], "--ui-seams") == 0) {
            seams = true;
        } else if (strcmp(argv[i], "--dash") == 0 && more) {
            double on = 0.0, off = 0.0;
            if (sscanf(argv[++i], "%lf,%lf", &on, &off) != 2) {
                fprintf(stderr, "canvas: --dash takes LENGTH,GAP in millimetres\n");
                return 2;
            }
            g_dash = (CanvasDash){MM(on), MM(off), 0};
        } else if ((strcmp(argv[i], "--cap") == 0 || strcmp(argv[i], "--join") == 0) && more) {
            // round, then butt or mitre, then square or bevel: the order of CanvasCap and
            // CanvasJoin.
            const bool cap = strcmp(argv[i], "--cap") == 0;
            const char* const* names = cap ? (const char* const[]){"round", "butt", "square"}
                                           : (const char* const[]){"round", "mitre", "bevel"};
            const char* name = argv[++i];
            int kind = -1;
            for (int k = 0; k < 3; k++)
                kind = strcmp(name, names[k]) == 0 ? k : kind;
            if (kind < 0) {
                fprintf(stderr, "canvas: %s takes %s, %s or %s\n", cap ? "--cap" : "--join",
                        names[0], names[1], names[2]);
                return 2;
            }
            *(cap ? &g_cap : &g_join) = kind;
        } else if (strcmp(argv[i], "--dash-min-px") == 0 && more) {
            dash_min_px = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--dim") == 0 && more) {
            if (sscanf(argv[++i], "%f,%f", &g_dim, &g_dim_room) != 2) {
                fprintf(stderr, "canvas: --dim takes STRENGTH,ROOM (points)\n");
                return 2;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    const Sheet* sheet = find_sheet(sheet_name);
    if (!sheet) {
        fprintf(stderr, "canvas: no sheet named '%s'\n", sheet_name);
        return 2;
    }

    EngineConfig cfg = {
        .title = "canvas", .width = width, .height = height, .headless = headless, .idle = idle};
    Engine* engine = create_engine(&cfg);
    if (!engine)
        return 1;
    engine->exit_after_frames = frames;
    engine_set_screenshot_path(engine, screenshot);
    if (input_script && !engine_set_input_script(engine, input_script)) {
        free_engine(engine);
        return 1;
    }
    g_idle_say = idle && headless;
    // An empty scene and a camera on it: the canvas draws over the frame, and
    // the frame wants a scene to be the frame of.
    Scene* scene = create_scene();
    engine_add_scene(engine, scene);
    engine_set_camera(engine, create_camera(&(CameraDesc){.position = {0.0f, 0.0f, 1.0f}}));
    engine_set_2d_preset(engine, scene);
    glm_vec3_zero(engine->clear_color);
    // A scene not loaded from a file has no texture directory: --idle-load names its file from
    // the working directory.
    if (g_idle_load)
        set_texture_pool_directory(scene->tex_pool, ".");
    engine->show_gui = g_gui_click;
    if (picture) {
        ShaderProgram* pass = create_post_pass_program("canvas_identity", IDENTITY_PASS);
        if (pass) {
            engine_add_program(engine, pass);
            postfx_add_pass(engine->postfx, POSTFX_AT_AFTER_TONEMAP, pass);
        }
    }

    g_canvas = create_canvas(engine, &(CanvasDesc){.tile_shift = tile_shift});
    if (!g_canvas) {
        free_engine(engine);
        return 1;
    }
    if (!isnan(dash_min_px))
        g_canvas->dash_min_px = dash_min_px;

    // Each view shows the sheet's width across its own share of the window.
    g_view_count = split ? 2 : 1;
    const double cx = isnan(view_x) ? sheet->centre_mm[0] : view_x;
    const double cy = isnan(view_y) ? sheet->centre_mm[1] : view_y;
    const double cw = isnan(view_w) ? sheet->width_mm : view_w;
    CanvasView* view = &g_views[0];
    g_side_width = history ? HISTORY_WIDTH : 0.0f;
    *view = (CanvasView){.points_per_unit =
                             ((double)width - g_side_width) / (double)g_view_count / (cw * 1e6),
                         .mirror_x = mirror,
                         .background = sheet->background,
                         .dim = g_dim,
                         .dim_to = 0xffffffffu,
                         .dim_room = g_dim_room};
    // The centre in whole units plus its fraction, from millimetres and the offset.
    const double ux = cx * 1e6, uy = cy * 1e6;
    view->centre = (CanvasPoint){(int64_t)floor(ux) + offset, (int64_t)floor(uy) + offset};
    view->fraction[0] = ux - floor(ux);
    view->fraction[1] = uy - floor(uy);
    lay_out_views(engine);

    Font* font = built_in_font(engine->text_renderer->font_pool, CANVAS_FONTS_LABEL);
    if (!font) {
        free_canvas(g_canvas);
        free_engine(engine);
        return 1;
    }
    SheetContext ctx = {.canvas = g_canvas,
                        .edit = create_canvas_edit(g_canvas),
                        .offset = (int64_t)offset,
                        .view = view,
                        .fb_scale = (double)engine->fb_width / (double)engine->win_width,
                        .probe = probe,
                        .edit_thread = edit_thread,
                        .engine = engine,
                        .font = font};
    sheet->build(&ctx);
    canvas_apply(g_canvas, ctx.edit);
    free_canvas_edit(ctx.edit);
    // The split's second view: the same place seen from the other side, with
    // the layers the sheet says that side shows.
    if (split) {
        view->active = ctx.active[0];
        g_views[1] = *view;
        g_views[1].mirror_x = !mirror;
        g_views[1].shown = ctx.under_shown;
        g_views[1].active = ctx.active[1];
        lay_out_views(engine);
    }
    g_only_view = only_view;
    g_budget = budget;
    g_probe_target = probe;
    g_probe = probe;

    // Under --ui, a panel of the game UI over the canvas, at the UI's order; under --history, the
    // side column, and under --palette the palette, given the real pointer and keyboard.
    g_engine = engine;
    g_home_view = g_views[0];
    UISystem* ui = NULL;
    if (with_ui || history || palette || font_sheet || editor_count || borders || seams) {
        ui = create_ui_system(engine);
        ui_set_font(ui, font, 18.0f);
        ui_attach(ui, engine);
    }
    if (editor_count) {
        if (!build_editor(ui, editor_files, editor_count, vim)) {
            free_ui_system(ui);
            free_canvas(g_canvas);
            free_engine(engine);
            return 1;
        }
        g_ui_live = ui;
    }
    if (font_sheet && !build_font_sheet(ui, engine, font)) {
        fprintf(stderr, "canvas: --font-sheet could not make its built-in font %s\n",
                canvas_fonts[CANVAS_FONTS_EDITOR].source);
        free_ui_system(ui);
        free_canvas(g_canvas);
        free_engine(engine);
        return 1;
    }
    if (palette) {
        build_palette(ui);
        g_ui_live = ui;
    }
    if (history) {
        Font* mono = built_in_font(engine->text_renderer->font_pool, CANVAS_FONTS_PIXEL);
        build_history(ui, mono);
        g_ui_live = ui;
        UIScreen* screen = ui_top(ui);
        ui_layout(screen, (float)width, (float)height);
        if (probe) {
            const double s = ctx.fb_scale;
            const UIElement* column = ui_screen_root(screen)->children[0];
            const UIElement* pane = column->children[0];
            printf("canvas history column");
            say_box(column->rect, s);
            printf(" pane");
            say_box(pane->rect, s);
            printf(" want 1a1c24 input %ld\n", lround(g_history_input->rect.y * s));
        }
    }
    if (with_ui) {
        UIScreen* screen = ui_screen(ui, "panel");
        UIElement* panel = ui_panel(ui_screen_root(screen));
        ui_set_size(panel, UI_FIXED, 160.0f, UI_FIXED, 90.0f);
        ui_set_style(panel, &(UIStyle){.bg = {0.2f, 0.6f, 0.9f, 1.0f}});
        if (g_ui_text)
            ui_label(panel, g_ui_text);
        // Under --ui-keys the panel is focused by a press and takes the keys, and a field under it
        // is something to move the pointer across while it holds the focus.
        if (g_ui_keys) {
            panel->focusable = true;
            g_keys_label = ui_label(panel, "click, then type");
            ui_set_on_key(panel, panel_keys, NULL);
            ui_text_field(ui_screen_root(screen), 0, NULL, NULL);
            ui_screen_set_modal(screen, false);
            g_ui_live = ui;
        }
        // Under --ui-enter, the panel fades in and a button in it takes focus, so the UI has a
        // screen entering and an ease to play out before it is still.
        if (ui_enter) {
            ui_screen_transition(screen, UI_TRANSITION_FADE, 0.25f);
            ui_set_focus(screen, ui_button(panel, "OK", NULL, NULL));
            g_ui_enter = ui;
        }
        ui_push(ui, screen);
        ui_layout(screen, (float)width, (float)height);
        if (probe) {
            printf("canvas ui panel");
            say_box(panel->rect, ctx.fb_scale);
            printf(" want 3399e6\n");
        }
    }
    if (borders)
        build_borders(ui, width, height, ctx.fb_scale, probe);
    if (seams)
        build_seams(ui, ctx.fb_scale, probe);
    fflush(stdout);

    engine_add_overlay(engine, draw_view, NULL, 0);
    engine_set_cursor_position_callback(engine, on_cursor);
    engine_set_mouse_button_callback(engine, on_button);
    engine_set_scroll_callback(engine, on_scroll);
    engine_set_key_callback(engine, on_key);
    if (g_refuse_close)
        engine_set_close_hook(engine, canvas_close, NULL);

    engine_run(engine, on_update, NULL, NULL);
    // How often the window was asked to close and refused it, and the frames the run drew.
    if (probe)
        printf("canvas close asked %d refused %d frames %zu\n", g_close_asked, g_close_asked > 0,
               engine->total_frames);
    if (g_idle_say)
        printf("canvas idle frames %zu waited %.6f\n", engine->total_frames, engine->time_waited);
    // The palette as the run left it: whether it is open, its text's length, the rows it holds,
    // the first row's item and that item's matched bytes; the rows built; and its field's box and
    // its second row's, while built, in framebuffer pixels.
    if (probe && g_palette) {
        printf("canvas palette state %d %zu %d %d", ui_on_stack(ui, g_palette) ? 1 : 0,
               strlen(ui_field_text(g_pal_field)), g_pal_count,
               g_pal_count ? g_pal_hits[0].index : -1);
        UITextRun lit[FUZZY_QUERY_MAX];
        const size_t n = g_pal_count ? palette_lit(pal_name(g_pal_hits[0].index), lit) : 0;
        for (size_t i = 0; i < n; i++)
            printf(" %zu", lit[i].start);
        printf("\n");
        printf("canvas palette list rows %d built %zu\n", g_pal_count, g_pal_list->child_count);
        printf("canvas palette field");
        say_box(g_pal_field->rect, ctx.fb_scale);
        printf("\n");
        const UIElement* second = ui_scroll_find_row(g_pal_list, 1);
        if (second) {
            printf("canvas palette second");
            say_box(second->rect, ctx.fb_scale);
            printf("\n");
        }
    }
    // The editor as the run left it, the focused pane's: its text, as hex, and its selections.
    if (probe && g_editor_sheet.file_count) {
        UIElement* pane = pane_focused();
        const TextDoc* doc = ui_editor_doc(pane);
        say_text("editor hex", doc);
        printf("canvas editor sel");
        say_selections(pane);
        printf("\n");
        // The parse's errors as they stand, and where the first is drawn, in framebuffer pixels.
        size_t count = 0;
        const TextRange* r = text_doc_ranges(doc, 0, &count);
        printf("canvas editor errors %zu", count);
        for (size_t i = 0; i < count; i++)
            printf(" %zu %zu", r[i].start, r[i].end);
        printf("\n");
        printf("canvas editor settled %d\n", ui_editor_settled(pane) ? 1 : 0);
        // What the texts refused, being read-only, and the clipboard, as hex.
        printf("canvas editor refused %d\n", g_editor_sheet.refused);
        const char* clip = engine_clipboard_get(engine);
        say_hex("editor clip", clip, clip ? strlen(clip) : 0);
        // With --vim: its state, and what the last line typed came to, as hex.
        if (g_editor_sheet.vim) {
            char what[32];
            snprintf(what, sizeof(what), "editor vim %d",
                     (int)ui_editor_vim_state(g_editor_sheet.vim));
            say_hex(what, g_editor_sheet.message, strlen(g_editor_sheet.message));
        }
        float ex = 0.0f, ey = 0.0f;
        if (count && ui_editor_point_of(pane, r[0].start, &ex, &ey))
            printf("canvas editor error_at %ld %ld\n", lround(ex * ctx.fb_scale),
                   lround(ey * ctx.fb_scale));
        // Every pane in the window's order -- the file it shows, the offset at its view's top and
        // its selections, the focused one marked -- the rows holding them, the pane the vim line
        // answers, then every file's text.
        int k = 0, asked = -1;
        say_panes(g_editor_sheet.area, &k, ui_focused(g_editor_sheet.screen));
        k = 0;
        printf("canvas panes tree ");
        say_tree(g_editor_sheet.area, &k, g_editor_sheet.asking, &asked);
        printf("\ncanvas panes asking %d\n", asked);
        for (int f = 0; f < g_editor_sheet.file_count; f++) {
            char name[32];
            snprintf(name, sizeof(name), "file %d", f);
            say_text(name, g_editor_sheet.files[f].doc);
        }
    }
    board_late_settle(true);
    free_ui_system(ui);
    free_ui_editor_vim(g_editor_sheet.vim);
    for (int i = 0; i < 3; i++)
        free_glyph_cache(g_font_sheet.caches[i]);
    free_glyph_cache(g_editor_sheet.face);
    for (int f = 0; f < g_editor_sheet.file_count; f++) {
        ParseJob* job = &g_editor_sheet.files[f].job;
        if (job->running) {
            cetra_thread_join(job->thread);
            free(job->text);
        }
        free_text_doc(g_editor_sheet.files[f].doc);
    }
    free_canvas(g_canvas);
    free_engine(engine);
    return 0;
}
