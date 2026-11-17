/*
 * Tags (spec 14.39), for both languages: the layout of a tag table's texels, which tag_table.c
 * writes and the canvas's shaders and tags.glsl read, and a 3D view's block and lanes, which
 * scene_view.c and render.c write and tags.glsl reads. Numbers only, as canvas_constants.glsl's;
 * the `u` suffix makes a flag unsigned in both.
 */

// A tag's texels in its table: its colour, its outline, then its dim, hidden, raised and lit.
#define TAG_TABLE_TEXELS  3
#define TAG_TEXEL_COLOUR  0
#define TAG_TEXEL_OUTLINE 1
#define TAG_TEXEL_MISC    2

// A view's class rows, a row a class; class 0 is none.
#define TAG_VIEW_CLASSES 256
// Its head's flags: it draws with tags; some surface of it drops in some pass, which a depth-only
// program reads the table to find; it draws some surface see-through; some tag has an outline.
#define TAG_VIEW_ON       1u
#define TAG_VIEW_DROPS    2u
#define TAG_VIEW_GHOSTS   4u
#define TAG_VIEW_OUTLINED 8u

// Which of a view's surfaces a draw keeps (uTagLane): every one, the solid, the see-through. A
// hidden surface is kept by none.
#define TAG_LANE_ALL    0
#define TAG_LANE_SOLID  1
#define TAG_LANE_GHOSTS 2
