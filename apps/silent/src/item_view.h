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

// Where every picture is drawn before it is resolved into its own: the program, and four samples
// as large as the largest picture asked of it, shared so a picture drawn once keeps none.
typedef struct ItemStage {
    ShaderProgram* program;
    GLuint fbo, colour, depth;
    int width, height;
} ItemStage;

// One picture, and what it is of: asked for the same again, it is not drawn again.
typedef struct ItemView {
    GLuint fbo;
    Texture texture; // the picture for the UI, which reads only its id; id 0 when it failed
    const SceneNode* model;
    ItemShot shot;
} ItemView;

// False, with the reason printed, if the program will not build; no picture is drawn then.
bool item_stage_start(ItemStage* stage);
void item_stage_free(ItemStage* stage);

// `model` as `shot` sees it, `width` x `height` pixels: the picture, or NULL when it cannot be
// made, which is not tried again until something asked for changes.
const Texture* item_view_draw(ItemView* view, ItemStage* stage, const SceneNode* model,
                              const ItemShot* shot, int width, int height);
void item_view_free(ItemView* view);

#endif // _SILENT_ITEM_VIEW_H_
