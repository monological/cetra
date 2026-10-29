#ifndef _CANVAS_H_
#define _CANVAS_H_

/*
 * The 2D canvas (spec 14.0): exact 2D shapes drawn after tone mapping, at the
 * display's own resolution, kept on the GPU and edited in place.
 *
 * UNITS. Every position and length is a 64-bit integer in the app's own unit
 * (a nanometre, say), with y up. Nothing converts them to floats until the
 * vertex stage, and then only relative to the view, so a shape drawn a long
 * way from the origin is drawn exactly as one at it.
 *
 * TILES. Shapes are kept in square tiles per layer, 2^tile_shift units on a
 * side, each a range of its layer's GPU buffer: an edit uploads only the tiles
 * it touched, and a tile out of view costs its instances a rejection in the
 * vertex stage and no fragment.
 *
 * EDITS. Shapes are added and removed through a CanvasEdit, which holds no GL
 * and may be filled on any thread; canvas_apply merges it on the main thread.
 *
 * TAGS. Each shape carries a net and an object, two indices into a table of
 * states (CanvasTags) that the shader reads: highlighting a net, selecting an
 * object or hiding one changes an entry and rebuilds no geometry. The object
 * is also what an edit removes a shape by.
 *
 * VIEWS. A view is a plain value -- a rectangle of the window, a pan, a zoom
 * and the layers it shows -- and canvas_draw draws one, from an engine overlay
 * (engine_add_overlay). Several views may draw one canvas in a frame.
 *
 * Colours are 0xRRGGBBAA, blended in display-encoded values as the UI is.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct Engine;

typedef struct CanvasPoint {
    int64_t x, y;
} CanvasPoint;

// The most layers a canvas holds: the bits of a CanvasLayerSet.
#define CANVAS_LAYER_MAX 256

// The tile size a canvas is made with when its desc leaves it 0: 2^22 units.
#define CANVAS_TILE_SHIFT 22

typedef struct CanvasLayer {
    // ENGINE-OWNED, read only.
    uint32_t index; // its bit in a CanvasLayerSet, in the order layers were added
    char* name;     // owned
    struct CanvasLayerData* data;

    // SETTINGS: plain stores. Write them directly, at any time.
    float opacity; // the layer as a whole; 1 when added
    int order;     // drawn lowest first, ties in the order added; its index when added
} CanvasLayer;

// A set of layers, by their index. Empty means every layer.
typedef struct CanvasLayerSet {
    uint64_t bits[CANVAS_LAYER_MAX / 64];
} CanvasLayerSet;

static inline void canvas_layer_set_add(CanvasLayerSet* set, const CanvasLayer* layer) {
    if (set && layer && layer->index < CANVAS_LAYER_MAX)
        set->bits[layer->index / 64] |= 1ull << (layer->index % 64);
}

static inline bool canvas_layer_set_has(const CanvasLayerSet* set, const CanvasLayer* layer) {
    if (!set || !layer || layer->index >= CANVAS_LAYER_MAX)
        return false;
    return (set->bits[layer->index / 64] >> (layer->index % 64)) & 1u;
}

static inline bool canvas_layer_set_empty(const CanvasLayerSet* set) {
    for (size_t i = 0; set && i < CANVAS_LAYER_MAX / 64; i++)
        if (set->bits[i])
            return false;
    return true;
}

/*
 * Tags: a state for each net and each object, shared by every canvas that
 * borrows the table, so one write shows in the schematic and the layout alike.
 * A tag is an index; keep them dense, since the table is as long as the
 * largest one set. Tag 0 means none and its state is never read.
 */
typedef struct CanvasTags CanvasTags;

typedef struct CanvasTagState {
    uint32_t colour;  // in place of the shape's style colour; 0 = the style's
    uint32_t outline; // a ring round a shape, for a candidate under the cursor; 0 = none
    float dim;        // 0 = as drawn, 1 = black
    bool hidden;
    bool raised; // wins where shapes of its layer overlap
} CanvasTagState;

CanvasTags* create_canvas_tags(void);
void free_canvas_tags(CanvasTags* tags);
// False, and nothing changed, for tag 0 or past the table's limit.
bool canvas_tags_set(CanvasTags* tags, uint32_t tag, CanvasTagState state);
CanvasTagState canvas_tags_get(const CanvasTags* tags, uint32_t tag);

/*
 * The canvas.
 */
typedef struct CanvasDesc {
    CanvasTags* tags; // borrowed and shared; NULL = a table of its own
    int tile_shift;   // log2 of a tile's side in units; 0 = CANVAS_TILE_SHIFT
} CanvasDesc;

typedef struct Canvas {
    // ENGINE-OWNED, read only.
    struct Engine* engine;
    uint64_t uploads;        // tile ranges uploaded, ever
    uint64_t bytes_uploaded; // geometry bytes uploaded, ever
    uint64_t generation;     // changes whenever what the canvas draws may have
    struct CanvasData* data;

    // SETTINGS: plain stores. Write them directly, at any time.
    float text_min_px;  // a string whose em is smaller is not drawn
    float shape_min_px; // a shape spanning less is not drawn; 0 = every shape
} Canvas;

// Made with no GL; the GL objects are made at the first draw. `desc` may be
// NULL.
Canvas* create_canvas(struct Engine* engine, const CanvasDesc* desc);
void free_canvas(Canvas* canvas);

// NULL past CANVAS_LAYER_MAX. The name is copied.
CanvasLayer* canvas_add_layer(Canvas* canvas, const char* name);

// A colour shapes are drawn in, by index. Changing one recolours every shape
// that names it. Style 0 is added with the canvas, white.
uint16_t canvas_add_style(Canvas* canvas, uint32_t rgba);
void canvas_set_style(Canvas* canvas, uint16_t style, uint32_t rgba);

CanvasTags* canvas_tags(const Canvas* canvas);

/*
 * Shapes. Each starts with what it is drawn with and what it belongs to.
 */
typedef struct CanvasPaint {
    CanvasLayer* layer;
    uint16_t style;
    uint32_t net;    // 0 = none
    uint32_t object; // 0 = none; what canvas_edit_remove_object removes by
} CanvasPaint;

// A line `width` wide between two points, its ends round unless flat.
typedef struct CanvasSegment {
    CanvasPaint paint;
    CanvasPoint a, b;
    int64_t width;
    bool flat_ends;
} CanvasSegment;

// A line `width` wide along a circle of `radius` from a to b, counter-clockwise
// unless clockwise, the shorter way unless major -- as SVG and pcb state one. A
// radius shorter than half the chord is taken as half the chord. Its ends are
// round unless flat.
typedef struct CanvasArc {
    CanvasPaint paint;
    CanvasPoint a, b;
    int64_t radius;
    int64_t width;
    bool clockwise;
    bool major;
    bool flat_ends;
} CanvasArc;

// Filled when stroke is 0, otherwise a ring `stroke` wide centred on the radius.
typedef struct CanvasCircle {
    CanvasPaint paint;
    CanvasPoint centre;
    int64_t radius;
    int64_t stroke;
} CanvasCircle;

// A rectangle size_x by size_y about its centre, its corners rounded by
// `corner`, turned by `angle` radians counter-clockwise. Filled when stroke is
// 0, otherwise its outline `stroke` wide centred on the edge.
typedef struct CanvasBox {
    CanvasPaint paint;
    CanvasPoint centre;
    int64_t size_x, size_y;
    int64_t corner;
    int64_t stroke;
    double angle;
} CanvasBox;

// Segments through `count` points with round ends, so they join round.
typedef struct CanvasPolyline {
    CanvasPaint paint;
    const CanvasPoint* points;
    size_t count;
    int64_t width;
    bool closed;
} CanvasPolyline;

/*
 * Edits: built on any thread, applied on the main thread. An edit is a list of
 * steps applied in order, so removing an object and adding it again replaces
 * it.
 */
typedef struct CanvasEdit CanvasEdit;

// Reads only the canvas's tile size, which never changes.
CanvasEdit* create_canvas_edit(const Canvas* canvas);
void free_canvas_edit(CanvasEdit* edit);

// Each is refused by name, and leaves the edit as it was, for a shape with no
// layer, a negative width or stroke, or no extent.
void canvas_edit_segment(CanvasEdit* edit, const CanvasSegment* segment);
void canvas_edit_arc(CanvasEdit* edit, const CanvasArc* arc);
void canvas_edit_circle(CanvasEdit* edit, const CanvasCircle* circle);
void canvas_edit_box(CanvasEdit* edit, const CanvasBox* box);
void canvas_edit_polyline(CanvasEdit* edit, const CanvasPolyline* line);
void canvas_edit_remove_object(CanvasEdit* edit, uint32_t object);
void canvas_edit_clear_layer(CanvasEdit* edit, CanvasLayer* layer);

// Merges the edit into the canvas, in time proportional to the edit, and
// empties it for reuse. The tiles it touched upload at the next draw.
void canvas_apply(Canvas* canvas, CanvasEdit* edit);

/*
 * Views.
 */
typedef struct CanvasView {
    float x, y, width, height; // the rectangle drawn into, in window points from the top left
    CanvasPoint centre;        // the unit at the rectangle's middle...
    double fraction[2];        // ...plus this much of the next, each in [0, 1)
    double points_per_unit;    // the zoom
    bool mirror_x;             // seen from the other side, as a board from below
    uint32_t background;       // drawn under every layer, opaque
    CanvasLayerSet shown;      // empty = every layer
    const CanvasLayer* active; // drawn last, over the others; NULL = none
} CanvasView;

// Draws the view into its rectangle of the frame. Call it from an overlay.
void canvas_draw(Canvas* canvas, const CanvasView* view);

// The unit under a window point, and the window point of a unit.
CanvasPoint canvas_view_unit_at(const CanvasView* view, float x, float y);
void canvas_view_point_of(const CanvasView* view, CanvasPoint p, float* x, float* y);
// Zooms by `factor` keeping the unit under the window point where it is.
void canvas_view_zoom_at(CanvasView* view, float x, float y, double factor);
// Moves what is drawn by (dx, dy) window points, as a drag does.
void canvas_view_pan(CanvasView* view, float dx, float dy);

#endif
