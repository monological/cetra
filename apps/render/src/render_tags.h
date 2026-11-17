#ifndef RENDER_TAGS_H
#define RENDER_TAGS_H

#include <stdbool.h>
#include <stdint.h>

#include "cetra/engine.h"
#include "cetra/scene_tags.h"
#include "cetra/scene_view.h"

/*
 * Tags from the command line (spec 14.39), for the gates and the look. A frame F: prefix makes a
 * write at that frame; without one it is made at the first.
 *
 *   --node-tag [F:]NAME=TAG[:CLASS]   the named node's tag and class
 *   --piece-tags NAME=FIRST           each separate piece of the node's first mesh a tag of its
 *                                     own, FIRST, FIRST+1, ... in the order the vertices meet them
 *   --tag [F:]TAG=SPEC                a row of the table every view borrows: SPEC is `none` or a
 *                                     comma list of colour=RRGGBBAA, outline=RRGGBBAA, dim=D,
 *                                     hidden, lit
 *   --tag-class [F:][V/]CLASS=SPEC    a class row, every view's or grid view V's: none, or
 *                                     colour=RRGGBBAA, see=S, flat=F, hidden
 *   --tag-fade [F:][V/]S              a view's fade
 *   --tag-table                       the table, borrowed, with nothing written
 *   --tag-views MASK                  the grid views that borrow the table, a bit each; all
 */

#define RENDER_TAGS_MAX 32

typedef struct RenderTags {
    struct {
        int frame;
        char name[64];
        uint32_t tag;
        uint8_t klass;
    } nodes[RENDER_TAGS_MAX];
    int node_count;
    struct {
        char name[64];
        uint32_t first;
    } pieces[RENDER_TAGS_MAX];
    int piece_count;
    struct {
        int frame;
        uint32_t tag;
        SceneTagState state;
    } rows[RENDER_TAGS_MAX];
    int row_count;
    struct {
        int frame, view; // view -1 = every view
        int klass;       // -1 = the fade, not a class
        SceneTagClass row;
        float fade;
    } classes[RENDER_TAGS_MAX];
    int class_count;
    bool table;
    unsigned table_views;
    bool table_views_set;
} RenderTags;

// 1 when argv[*i] is one of the flags above, its argument consumed; 0 when it is not; -1, said,
// when it is malformed.
int render_tags_parse(RenderTags* tags, int argc, char** argv, int* i);
// This frame's part: the writes due, the classes and fades each view carries, the table each
// borrows. After the grid's views have taken the driver's settings for the frame.
void render_tags_frame(const RenderTags* tags, Engine* engine, SceneView* driver, SceneView** grid,
                       int grid_count, int frame);
// The flags' usage lines.
void render_tags_usage(void);

#endif // RENDER_TAGS_H
