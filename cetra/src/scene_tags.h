#ifndef _SCENE_TAGS_H_
#define _SCENE_TAGS_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Tags in 3D (spec 14.39): a state for each object a scene's surfaces are of, read by every view
 * that borrows the table (SceneView.tags), so one write shows in each. An object is a vertex's tag
 * (Mesh.tags), else its node's (SceneNode.tag), else an ancestor's. A tag is an index; keep them
 * dense, since the table is as long as the largest one set. Tag 0 means none and has no state;
 * any other never set, past the table or not, is in a state of all zeros, drawn as it is.
 *
 * A state says how a tag is drawn, not what it means: what is chosen, found or marked is the app's
 * to say in a colour or an outline.
 */
typedef struct SceneTags SceneTags;

typedef struct SceneTagState {
    // A matte paint in place of the material's colour, 0xRRGGBBAA in the display's sRGB, the
    // alpha its share; 0 = the material's. Over a class's colour (SceneTagClass).
    uint32_t colour;
    // A ring round the silhouette, drawn over the view's finished picture, so a change restarts
    // no still view; full where the surface is seen, fainter where it is hidden. 0 = none.
    uint32_t outline;
    float dim;   // 0 = as drawn, 1 = black
    bool hidden; // drawn in no pass, shadows included
    bool lit;    // spared by a view's fade (SceneViewSettings.tag_fade)
} SceneTagState;

SceneTags* create_scene_tags(void);
void free_scene_tags(SceneTags* tags);
// False, and nothing changed, for tag 0 or past the table's limit.
bool scene_tags_set(SceneTags* tags, uint32_t tag, SceneTagState state);
SceneTagState scene_tags_get(const SceneTags* tags, uint32_t tag);

#endif // _SCENE_TAGS_H_
