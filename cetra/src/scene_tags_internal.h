#ifndef _SCENE_TAGS_INTERNAL_H_
#define _SCENE_TAGS_INTERNAL_H_

#include <stdbool.h>
#include <stdint.h>

#include "scene_tags.h"

// The engine's side of a scene's tags, read through the views borrowing them. NULL tags are
// none: no write, no outline, nothing hidden.

// Writes that changed how a tag is drawn, counted apart from those that changed only its outline.
uint64_t scene_tags_generation(const SceneTags* tags);
uint64_t scene_tags_outline_generation(const SceneTags* tags);
// Whether any tag has an outline; and whether any from `lo` to `hi` may, false only where none
// does.
bool scene_tags_outlined(const SceneTags* tags);
bool scene_tags_outlined_within(const SceneTags* tags, uint32_t lo, uint32_t hi);
bool scene_tags_hides(const SceneTags* tags);
// The table brought up to date on the GPU and bound to `unit`; its count, one past the largest tag
// set, which the shader reads no tag past. 0 for NULL tags, with nothing bound.
uint32_t scene_tags_bind(SceneTags* tags, int unit);

#endif // _SCENE_TAGS_INTERNAL_H_
