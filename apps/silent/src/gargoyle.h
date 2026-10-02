#ifndef _SILENT_GARGOYLE_H_
#define _SILENT_GARGOYLE_H_

#include "kit.h"

/*
 * A gargoyle crouched on its spout stone (spec 13.13), in stone, about 1.2 m from the wall to
 * its jaws. In frame `f` it faces along +d with its back to the wall at d = 0 and its perch's
 * top 0.27 m below the frame's origin. `mouth` gets where the rain it carries pours from.
 */
void gargoyle_build(Kit* kit, const KitFrame* f, vec3 mouth);

#endif // _SILENT_GARGOYLE_H_
