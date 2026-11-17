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

// The engine's side, read through a view. Writes that changed how a tag is drawn, counted apart
// from those that changed only its outline, which restart nothing.
uint64_t scene_tags_generation(const SceneTags* tags);
uint64_t scene_tags_outline_generation(const SceneTags* tags);
// Whether any tag has an outline: a view's outline pass runs only then.
bool scene_tags_outlined(const SceneTags* tags);
// The table brought up to date on the GPU and bound to `unit`; its count, one past the largest tag
// set, which the shader reads no tag past. 0 for NULL tags, with nothing bound.
uint32_t scene_tags_bind(SceneTags* tags, int unit);

#endif // _SCENE_TAGS_H_
