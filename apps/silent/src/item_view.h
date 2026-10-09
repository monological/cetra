#ifndef _SILENT_ITEM_VIEW_H_
#define _SILENT_ITEM_VIEW_H_

#include <stdbool.h>
#include <GL/glew.h>

#include "cetra/program.h"
#include "cetra/scene.h"
#include "cetra/texture.h"

/*
 * Pictures of an item for the backpack's screen (spec 13.40): one item, a node on no scene graph,
 * drawn alone into a picture of its own -- its own light, its own camera, four samples resolved,
 * a clear background -- for the UI to show as a texture. The engine draws one view of one world;
 * an item held up in that world would be fogged, darkened by the night's exposure and cut by
 * whatever wall stood behind the player.
 *
 * It draws with raw GL from inside the UI's overlay, so it puts back every piece of state it
 * touches.
 */

// How an item is seen.
typedef struct ItemShot {
    float angle; // its turn about the vertical, radians
    float tilt;  // the camera's lift over it, radians
    // An icon: orthographic, framed to its box as turned, so a long thing fills a long footprint.
    // Otherwise in perspective, framed to the sphere round it, so it stays in frame as it turns.
    bool flat;
} ItemShot;

// One picture, made again only when its size changes.
typedef struct ItemView {
    GLuint ms_fbo, ms_colour, ms_depth; // where it draws, multisampled
    GLuint fbo, colour;                 // what the UI shows, resolved
    int width, height;
    Texture texture; // wraps `colour` for the UI, which reads only its id
} ItemView;

// The program every picture is drawn with; NULL, with the reason printed, if it will not build.
ShaderProgram* create_item_view_program(void);
// `model` as `shot` sees it, `width` x `height` pixels: the picture, or NULL when it cannot be
// made.
const Texture* item_view_draw(ItemView* view, ShaderProgram* program, const SceneNode* model,
                              const ItemShot* shot, int width, int height);
// Whether it holds a picture this size.
bool item_view_ready(const ItemView* view, int width, int height);
void item_view_free(ItemView* view);

#endif // _SILENT_ITEM_VIEW_H_
