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
#include "cetra/internal/ext/stb_truetype.h"
#include "cetra/internal/thread.h"
#include "cetra/postfx.h"
#include "cetra/program.h"
#include "cetra/scene.h"
#include "cetra/text.h"
#include "cetra/triangulate.h"
#include "cetra/ui.h"

// Dear ImGui via cimgui, for --gui-click's click handed to the debug GUI.
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"

#define MM(v) ((int64_t)llround((v) * 1e6))

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
    const CanvasPaint cu = {.layer = copper, .style = orange};
    const CanvasPaint sk = {.layer = silk, .style = white};
#define AT(x, y) ((CanvasPoint){MM(x) + offset, MM(y) + offset})

    // Segments: widths from 0.02 mm to 1 mm, round ends, then flat.
    const double widths[] = {0.02, 0.05, 0.1, 0.25, 0.5, 1.0};
    for (int i = 0; i < 6; i++) {
        const double x = -10.0 + i * 2.2;
        canvas_edit_segment(
            edit, &(CanvasSegment){
                      .paint = cu, .a = AT(x, 5.0), .b = AT(x + 1.4, 7.0), .width = MM(widths[i])});
        canvas_edit_segment(edit, &(CanvasSegment){.paint = cu,
                                                   .a = AT(x + 0.8, 5.0),
                                                   .b = AT(x + 1.6, 5.6),
                                                   .width = MM(widths[i]),
                                                   .flat_ends = true});
    }

    // Arcs: a quarter, a major arc, a clockwise one, flat ends, a half turn.
    const CanvasPaint bl = {.layer = copper, .style = blue};
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
    canvas_edit_arc(edit, &(CanvasArc){.paint = bl,
                                       .a = AT(-1.0, 1.5),
                                       .b = AT(-2.0, 2.5),
                                       .radius = MM(1.0),
                                       .width = MM(0.4),
                                       .flat_ends = true});
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
    const CanvasPaint gr = {.layer = copper, .style = green};
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
    return (CanvasPaint){.layer = canvas_add_layer(canvas, "white"),
                         .style = canvas_add_style(canvas, 0xffffffffu)};
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
    canvas_edit_segment(
        edit,
        &(CanvasSegment){.paint = paint, .a = ba, .b = bb, .width = MM(0.2), .flat_ends = true});
    PROBE("bar", c, "len %.6f w %.6f", hypot((double)(bb.x - ba.x), (double)(bb.y - ba.y)) * s,
          MM(0.2) * s);

    // Three quarters of a circle with flat ends, clockwise from south round to east: an arc of
    // more than half a turn, whose flat ends' inner ramp is measured only near each end.
    c = CELL(4, 0);
    canvas_edit_arc(edit, &(CanvasArc){.paint = paint,
                                       .a = {c.x, c.y - MM(0.5)},
                                       .b = {c.x + MM(0.5), c.y},
                                       .radius = MM(0.5),
                                       .width = MM(0.1),
                                       .clockwise = true,
                                       .major = true,
                                       .flat_ends = true});
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
    const CanvasPaint fill = {.layer = body, .style = canvas_add_style(canvas, 0xffffc2ffu)};
    const CanvasPaint line = {.layer = graphics, .style = canvas_add_style(canvas, 0x840000ffu)};
    const CanvasPaint name = {.layer = text, .style = canvas_add_style(canvas, 0x008484ffu)};
    const CanvasPaint number = {.layer = text, .style = canvas_add_style(canvas, 0xa90000ffu)};
    const CanvasPaint label = {.layer = text, .style = canvas_add_style(canvas, 0x404040ffu)};
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
static void probe_pixel(const SheetContext* ctx, const char* name, CanvasPoint p, uint32_t rgb) {
    double x, y;
    fb_of(ctx, p, &x, &y);
    printf("canvas tags %s at %ld %ld want %06x\n", name, lround(x), lround(y), rgb);
}

static void sheet_tags(SheetContext* ctx) {
    Canvas* canvas = ctx->canvas;
    CanvasEdit* edit = ctx->edit;
    CanvasTags* tags = canvas_tags(canvas);
    CanvasLayer* layer = canvas_add_layer(canvas, "tags");
    const uint16_t red = canvas_add_style(canvas, 0xff0000ffu);
    const uint16_t white = canvas_add_style(canvas, 0xffffffffu);
    const int64_t o = ctx->offset;
#define CELL(i) ((CanvasPoint){MM(-3.5 + 1.75 * ((i) % 5)) + o, MM(1.0 - 2.0 * ((i) / 5)) + o})
#define BOX(i, style_, net_, object_)                                           \
    canvas_edit_box(edit, &(CanvasBox){.paint = {layer, style_, net_, object_}, \
                                       .centre = CELL(i),                       \
                                       .size_x = MM(1.0),                       \
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
    canvas_edit_box(edit, &(CanvasBox){.paint = {layer, red, 0, 5},
                                       .centre = {c5.x + MM(0.5) + nudge, c5.y},
                                       .size_x = MM(1.0),
                                       .size_y = MM(1.0)});
    canvas_tags_set(tags, 6, (CanvasTagState){.colour = 0x0000ffffu});
    BOX(6, red, 0, 0);
    canvas_edit_box(edit, &(CanvasBox){.paint = {layer, red, 0, 6},
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
    canvas_edit_polyline(
        edit, &(CanvasPolyline){
                  .paint = {layer, red, 0, 8}, .points = trace, .count = 3, .width = MM(0.3)});
#undef BOX
    if (ctx->probe) {
        // A point well inside each box, and the outline's ring: a box's edge is
        // 0.5 mm from its centre, and its ring 1.75 points past that.
        const double ring_mm = 1.75 / (ctx->view->points_per_unit * 1e6);
        probe_pixel(ctx, "style", CELL(0), 0xff0000);
        probe_pixel(ctx, "net-colour", CELL(1), 0x00ff00);
        probe_pixel(ctx, "object-over-net", CELL(2), 0x0000ff);
        probe_pixel(ctx, "dim", CELL(3), 0x808080);
        probe_pixel(ctx, "hidden", CELL(4), 0x000000);
        probe_pixel(ctx, "raised", (CanvasPoint){c5.x + MM(0.25), c5.y}, 0x0000ff);
        printf("canvas tags raised-fringe at %ld %ld want %06x\n", (long)floor(edge_x),
               (long)floor(edge_y), 0xff0000u);
        probe_pixel(ctx, "first-added", (CanvasPoint){c6.x + MM(0.25), c6.y}, 0xff0000);
        probe_pixel(ctx, "outline-body", CELL(7), 0xff0000);
        probe_pixel(ctx, "outline-ring", (CanvasPoint){CELL(7).x + MM(0.5 + ring_mm), CELL(7).y},
                    0xffff00);
        // On the second segment's centre line, as far from the joint as the
        // first segment's ring is from its end: that ring passes through here,
        // inside the second segment's body.
        const double along = 0.15 + ring_mm, unit = sqrt(0.6 * 0.6 + 0.4 * 0.4);
        probe_pixel(ctx, "outline-joint",
                    (CanvasPoint){c8.x + MM(along * 0.6 / unit), c8.y - MM(along * 0.4 / unit)},
                    0xff0000);
    }
#undef CELL
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
    canvas_edit_box(edit, &(CanvasBox){.paint = {layer, style, net, object},
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
    const CanvasCircle pad = {.paint = {b->top, b->top_s, net, object},
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
    canvas_edit_text(edit, &(CanvasText){.paint = {b->silk, b->silk_s, 0, 0},
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
    canvas_edit_polyline(edit, &(CanvasPolyline){.paint = {b->edge, b->edge_s, 0, 0},
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
            board_route(edit, b, (CanvasPaint){b->top, b->top_s, pin, 300 + pin}, side, route, 4);
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
                    edit, &(CanvasCircle){.paint = {b->bottom, b->bottom_s, pin, 600 + pin},
                                          .centre = board_at(b, tx, ty),
                                          .radius = MM(0.45)});
            }
            board_route(edit, b, (CanvasPaint){b->bottom, b->bottom_s, pin, 500 + pin}, side,
                        bottom, bn);
        }
    }
    canvas_edit_box(edit, &(CanvasBox){.paint = {b->silk, b->silk_s, 0, 0},
                                       .centre = board_at(b, 0.0, 0.0),
                                       .size_x = MM(7.0),
                                       .size_y = MM(7.0),
                                       .stroke = MM(0.15)});
    canvas_edit_circle(edit, &(CanvasCircle){.paint = {b->silk, b->silk_s, 0, 0},
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
        canvas_edit_box(edit, &(CanvasBox){.paint = {b->silk, b->silk_s, 0, 0},
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
    canvas_edit_polygon(edit, &(CanvasPolygon){.paint = {b->plane, b->plane_s, BOARD_GND, 0},
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
 * and applies it on the frame its end wakes.
 */
enum { BOARD_LATE_MS = 50 };
static CanvasEdit* g_late_edit;  // the board's edit, until it is applied
static atomic_bool g_late_ready; // the thread has filled it
static bool g_late_thread_live;  // a thread was started and not yet joined
static cetra_thread_t g_late_thread;

static void* board_late_thread(void* engine) {
    cetra_sleep_ms(BOARD_LATE_MS);
    board_fill(g_late_edit, &g_board);
    atomic_store(&g_late_ready, true);
    engine_end_work(engine);
    return NULL;
}

static void board_late_start(Engine* engine) {
    g_late_thread_live = cetra_thread_create(&g_late_thread, board_late_thread, engine);
    if (!g_late_thread_live)
        board_late_thread(engine);
}

// Applies the board once its thread has filled it, or at exit only joins the thread, which may
// still be filling it, and drops it: a thread outliving the engine would end its work on freed
// memory.
static void board_late_settle(bool at_exit) {
    if (!g_late_edit || (!at_exit && !atomic_load(&g_late_ready)))
        return;
    if (g_late_thread_live)
        cetra_thread_join(g_late_thread);
    g_late_thread_live = false;
    if (!at_exit) {
        board_finish(g_canvas, g_late_edit);
        canvas_apply(g_canvas, g_late_edit);
    }
    free_canvas_edit(g_late_edit);
    g_late_edit = NULL;
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
            canvas_edit_segment(edit, &(CanvasSegment){.paint = {b->top, b->top_s, 0, 9001},
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
                    edit, &(CanvasSegment){.paint = {b->transient, b->wire_s, 0, 0},
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
        // Begun here, before the first frame can be the last; the thread is started late.
        g_late_edit = create_canvas_edit(canvas);
        engine_begin_work(ctx->engine);
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
    {"board", sheet_board, {0.0, 0.0}, 66.0, 0x001023ffu},
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

// Under --idle: the timed wakes --idle-wake asks for, one each frame in turn,
// and, headless, a line a frame and one at exit for the `idle` gate group.
enum { IDLE_WAKES_MAX = 8 };
static double g_wakes[IDLE_WAKES_MAX];
static int g_wake_count, g_wake_next;
static bool g_idle_say;
static const char* g_idle_load; // --idle-load: a texture asked for as late work
static bool g_late_started;     // the late work has been started
static bool g_gui_click;        // --gui-click: a click handed to the debug GUI on the first frame
static UISystem* g_ui_enter;    // --ui-enter: the UI, given an input pass of nothing each frame
static bool g_say_typed;        // --canvas-probe: say what each frame's input was given to type
static const char* g_ui_text;   // --ui-text: a label in the --ui panel
static UISystem* g_ui_live;     // the UI given the real pointer and keyboard each frame
// The pointer's button as the callbacks saw it since the last frame, for the UI's input pass.
static bool g_ptr_down, g_ptr_pressed, g_ptr_released;
static int g_ptr_mods; // the modifiers held at the last press

// The UI's input pass with the real pointer and keyboard; the presses and releases seen since the
// last frame are spent.
static void live_ui_update(Engine* engine) {
    if (!g_ui_live)
        return;
    UIInput in = {.pointer_x = g_last_x,
                  .pointer_y = g_last_y,
                  .pointer_down = g_ptr_down,
                  .pointer_pressed = g_ptr_pressed,
                  .pointer_released = g_ptr_released,
                  .pointer_mods = g_ptr_mods};
    ui_input_add_keyboard(&in, engine);
    ui_update(g_ui_live, &in, (float)engine->win_width, (float)engine->win_height);
    g_ptr_pressed = g_ptr_released = false;
}

// What the UI is handed to type this frame, through the helper an app uses, one token an event:
// `c<hex>` a character, `k<key>+<mods>` a key.
static void say_typed(const Engine* engine) {
    UIInput in = {0};
    ui_input_add_keyboard(&in, engine);
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
    if (g_late_started || g_wake_next < g_wake_count)
        return;
    g_late_started = true;
    if (g_late_edit)
        board_late_start(engine);
    if (g_idle_load)
        engine_load_texture(engine, engine_get_scene(engine)->tex_pool, g_idle_load,
                            texture_desc(true), on_idle_loaded, NULL);
}

static void history_stream(Engine* engine);

static void on_update(Engine* engine, float dt) {
    (void)dt;
    board_late_settle(false);
    if (g_idle_say)
        printf("canvas idle frame %zu delta %.6f\n", engine->total_frames + 1, engine->delta_time);
    start_late_work(engine);
    if (g_wake_next < g_wake_count)
        engine_request_frame_after(engine, g_wakes[g_wake_next++]);
    if (g_say_typed)
        say_typed(engine);
    live_ui_update(engine);
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

static void to_points(const Engine* engine, double fb_x, double fb_y, float* x, float* y) {
    *x = (float)(fb_x * engine->win_width / engine->fb_width);
    *y = (float)((engine->fb_height - fb_y) * engine->win_height / engine->fb_height);
}

static void on_cursor(Engine* engine, double fb_x, double fb_y) {
    float x, y;
    to_points(engine, fb_x, fb_y, &x, &y);
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
    if (button != GLFW_MOUSE_BUTTON_LEFT)
        return;
    if (action == GLFW_PRESS)
        g_ptr_mods = mods;
    g_ptr_down = action == GLFW_PRESS;
    g_ptr_pressed |= action == GLFW_PRESS;
    g_ptr_released |= action == GLFW_RELEASE;
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
// The history, the app's own: each message's text, whether the agent said it, and whether a part
// it names is set in the second face. The pane builds rows from it.
static char* g_msgs[HISTORY_MAX];
static bool g_msg_agent[HISTORY_MAX], g_msg_mono[HISTORY_MAX];
static int g_msg_count;
static int g_reply = -1;  // the message being streamed into, or -1
static size_t g_reply_at; // how much of HISTORY_REPLY is in it

// A message at the end of the history, and a row for it; its index, or -1 when the history is full.
static int history_push(bool agent, const char* said, bool mono) {
    if (g_msg_count >= HISTORY_MAX)
        return -1;
    char text[512];
    snprintf(text, sizeof(text), "%s: %s", agent ? "agent" : "you", said);
    g_msgs[g_msg_count] = strdup(text);
    g_msg_agent[g_msg_count] = agent;
    g_msg_mono[g_msg_count] = mono;
    if (g_history_pane && g_history_pane->rows)
        ui_scroll_set_row_count(g_history_pane, g_msg_count + 1);
    return g_msg_count++;
}

/*
 * A message's row: a read-only field, so its text can be selected and copied, wrapped across the
 * column, in the column's colour with no border, the speaker in a colour and a part it names in
 * the second face.
 */
static void build_message(UIElement* row, int index, void* user) {
    (void)user;
    UIElement* f = ui_text_field(row, UI_FIELD_MULTILINE | UI_FIELD_READ_ONLY, NULL, NULL);
    ui_set_size(f, UI_GROW, 0.0f, UI_FIT, 0.0f);
    f->max_rows = 1000;
    ui_set_style(f, &(UIStyle){.bg = {0.10f, 0.11f, 0.14f, 1.0f},
                               .border = {0.10f, 0.11f, 0.14f, 1.0f},
                               .padding = {1.0f, 1.0f, 1.0f, 1.0f}});
    const char* text = g_msgs[index];
    ui_field_set_text(f, text);
    const bool agent = g_msg_agent[index];
    UITextRun runs[2] = {
        {.start = 0,
         .length = agent ? 5 : 3,
         .color = {agent ? 0.45f : 0.55f, agent ? 0.70f : 0.85f, agent ? 1.0f : 0.55f, 1.0f}}};
    size_t count = 1;
    const char* part = strstr(text, "C12");
    if (g_mono && g_msg_mono[index] && part)
        runs[count++] = (UITextRun){.start = (size_t)(part - text), .length = 3, .font = g_mono};
    ui_set_text_runs(f, runs, count);
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
// the frame it will be drawn in. Its row is built again with each word.
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
    char text[520];
    snprintf(text, sizeof(text), "agent: %.*s", (int)g_reply_at, HISTORY_REPLY);
    free(g_msgs[g_reply]);
    g_msgs[g_reply] = strdup(text);
    ui_scroll_rows_changed(g_history_pane, g_reply, 1);
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
    ui_set_text(g_history_input, "Message");
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
    const char* query = ui_field_text(g_pal_field);
    size_t at[FUZZY_QUERY_MAX];
    size_t n = 0;
    if (!query[0] || fuzzy_match(query, name, at, FUZZY_QUERY_MAX) < 0)
        return 0;
    for (const unsigned char* p = (const unsigned char*)query; *p && n < FUZZY_QUERY_MAX; n++) {
        text_utf8_next(&p);
        const unsigned char* end = (const unsigned char*)name + at[n];
        text_utf8_next(&end);
        lit[n] = (UITextRun){.start = at[n],
                             .length = (size_t)(end - ((const unsigned char*)name + at[n])),
                             .color = {1.0f, 0.78f, 0.30f, 1.0f}};
    }
    return n;
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
    ui_label(row, g_pal_jump ? "" : COMMANDS[item].key);
}

/*
 * The list for what is in the field: every item that matches, best first and in the list's own
 * order among equals, a row each. The pane is as tall as the rows up to PALETTE_ROWS of them and
 * scrolls past that, so a short list takes only its own room.
 */
static void palette_filter(void) {
    const char* query = ui_field_text(g_pal_field);
    const int total = g_pal_jump ? JUMP_NAMES : COMMAND_COUNT;
    int found = 0;
    for (int i = 0; i < total; i++) {
        const int score = fuzzy_match(query, pal_name(i), NULL, 0);
        if (score >= 0)
            g_pal_hits[found++] = (Hit){i, score};
    }
    qsort(g_pal_hits, (size_t)found, sizeof(Hit), hit_order);
    g_pal_count = found;
    g_pal_selected = 0;
    if (found > PALETTE_ROWS)
        ui_set_size(g_pal_list, UI_GROW, 0.0f, UI_FIXED, PALETTE_ROWS * PALETTE_ROW_HEIGHT);
    else
        ui_set_size(g_pal_list, UI_GROW, 0.0f, UI_FIT, 0.0f);
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
    if (g_pal_jump)
        printf("canvas jump %s\n", g_jump_names[item]);
    else
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
    ui_field_set_on_key(g_pal_field, on_palette_key, NULL);
    // The rows in a pane with no gaps between them. A spacing of zero is the theme's, so a
    // thousandth of a point stands for none.
    g_pal_list = ui_scroll(box);
    g_pal_list->spacing = 0.001f;
}

static void palette_open(bool jump) {
    g_pal_jump = jump;
    ui_set_text(g_pal_field, jump ? "Jump to a part or net" : "Type a command");
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
#ifdef __APPLE__
    const bool shortcut = (mods & GLFW_MOD_SUPER) != 0;
#else
    const bool shortcut = (mods & GLFW_MOD_CONTROL) != 0;
#endif
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

static void usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s [-x] [-f frames] [-S out.ppm] [-W points] [-H points]\n"
            "          [--sheet NAME] [--view X,Y,WIDTH (mm)] [--offset UNITS] [--mirror]\n"
            "          [--tile-shift N] [--canvas-probe] [--picture] [--edit-thread]\n"
            "          [--split] [--only-view 0|1] [--ui] [--budget]\n"
            "          [--idle] [--idle-wake S,...] [--idle-load PATH] [--input-script PATH]\n"
            "          [--ui-enter] [--ui-text TEXT] [--gui-click] [--history] [--palette]\n",
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
    bool split = false, with_ui = false, budget = false, idle = false, ui_enter = false;
    bool history = false, palette = false;
    int frames = 0, width = 960, height = 600, tile_shift = 0, only_view = -1;
    const char* screenshot = NULL;
    const char* input_script = NULL;
    const char* sheet_name = "shapes";
    double view_x = NAN, view_y = NAN, view_w = NAN;
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
            // Seconds, comma-separated: each asked for on its frame in turn.
            for (const char* tok = strtok(argv[++i], ","); tok && g_wake_count < IDLE_WAKES_MAX;
                 tok = strtok(NULL, ","))
                g_wakes[g_wake_count++] = atof(tok);
        } else if (strcmp(argv[i], "--input-script") == 0 && more) {
            input_script = argv[++i];
        } else if (strcmp(argv[i], "--idle-load") == 0 && more) {
            g_idle_load = argv[++i];
        } else if (strcmp(argv[i], "--ui-enter") == 0) {
            with_ui = true;
            ui_enter = true;
        } else if (strcmp(argv[i], "--history") == 0) {
            history = true;
        } else if (strcmp(argv[i], "--palette") == 0) {
            palette = true;
        } else if (strcmp(argv[i], "--ui-text") == 0 && more) {
            with_ui = true;
            g_ui_text = argv[++i];
        } else if (strcmp(argv[i], "--gui-click") == 0) {
            g_gui_click = true;
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
                         .background = sheet->background};
    // The centre in whole units plus its fraction, from millimetres and the offset.
    const double ux = cx * 1e6, uy = cy * 1e6;
    view->centre = (CanvasPoint){(int64_t)floor(ux) + offset, (int64_t)floor(uy) + offset};
    view->fraction[0] = ux - floor(ux);
    view->fraction[1] = uy - floor(uy);
    lay_out_views(engine);

    Font* font = load_font(engine->text_renderer->font_pool, "apps/splash/assets/Roboto-Bold.ttf",
                           64.0f, true);
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
    g_say_typed = probe;

    // Under --ui, a panel of the game UI over the canvas, at the UI's order; under --history, the
    // side column, and under --palette the palette, given the real pointer and keyboard.
    g_engine = engine;
    g_home_view = g_views[0];
    UISystem* ui = NULL;
    if (with_ui || history || palette) {
        ui = create_ui_system(engine);
        ui_set_font(ui, font, 18.0f);
        ui_attach(ui, engine);
    }
    if (palette) {
        build_palette(ui);
        g_ui_live = ui;
    }
    if (history) {
        Font* mono = load_font(engine->text_renderer->font_pool,
                               "apps/splash/assets/PressStart2P.ttf", 64.0f, true);
        build_history(ui, mono);
        g_ui_live = ui;
        UIScreen* screen = ui_top(ui);
        ui_layout(screen, (float)width, (float)height);
        if (probe) {
            const double s = ctx.fb_scale;
            const UIElement* column = ui_screen_root(screen)->children[0];
            const UIElement* pane = column->children[0];
            printf("canvas history column %ld %ld %ld %ld pane %ld %ld %ld %ld want 1a1c24 "
                   "input %ld\n",
                   lround(column->rect.x * s), lround(column->rect.y * s),
                   lround((column->rect.x + column->rect.w) * s),
                   lround((column->rect.y + column->rect.h) * s), lround(pane->rect.x * s),
                   lround(pane->rect.y * s), lround((pane->rect.x + pane->rect.w) * s),
                   lround((pane->rect.y + pane->rect.h) * s), lround(g_history_input->rect.y * s));
        }
    }
    if (with_ui) {
        UIScreen* screen = ui_screen(ui, "panel");
        UIElement* panel = ui_panel(ui_screen_root(screen));
        ui_set_size(panel, UI_FIXED, 160.0f, UI_FIXED, 90.0f);
        ui_set_style(panel, &(UIStyle){.bg = {0.2f, 0.6f, 0.9f, 1.0f}});
        if (g_ui_text)
            ui_label(panel, g_ui_text);
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
            const double s = ctx.fb_scale;
            const UIRect r = panel->rect;
            printf("canvas ui panel %ld %ld %ld %ld want 3399e6\n", lround(r.x * s),
                   lround(r.y * s), lround((r.x + r.w) * s), lround((r.y + r.h) * s));
        }
    }
    fflush(stdout);

    engine_add_overlay(engine, draw_view, NULL, 0);
    engine_set_cursor_position_callback(engine, on_cursor);
    engine_set_mouse_button_callback(engine, on_button);
    engine_set_scroll_callback(engine, on_scroll);
    engine_set_key_callback(engine, on_key);

    engine_run(engine, on_update, NULL, NULL);
    if (g_idle_say)
        printf("canvas idle frames %zu waited %.6f\n", engine->total_frames, engine->time_waited);
    // The palette as the run left it: whether it is open, its text's length, the rows it holds,
    // the first row's item and that item's matched bytes; the rows built; and its field's box in
    // framebuffer pixels.
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
        const double s = ctx.fb_scale;
        const UIRect r = g_pal_field->rect;
        printf("canvas palette field %ld %ld %ld %ld\n", lround(r.x * s), lround(r.y * s),
               lround((r.x + r.w) * s), lround((r.y + r.h) * s));
    }
    board_late_settle(true);
    free_ui_system(ui);
    free_canvas(g_canvas);
    free_engine(engine);
    return 0;
}
