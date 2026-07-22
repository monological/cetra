#ifndef _GRASS_H_
#define _GRASS_H_

#include <stdbool.h>

#include "cetra/mesh.h"

// Wind-blown grass, built as one merged mesh -- the whole field is a single
// draw call, the same way the tree's bark and leaves are.
//
// Blades ride the engine's vegetation wind (material wind_mode 2) and use its
// existing per-vertex contract unchanged:
//   UV1.x = per-tuft phase, so a tuft moves as a unit and its neighbours do not
//   UV1.y = flex, 0 at the root so blades stay planted, 1 at the tip
//   UV0.y = position along the blade, which the leaf mode uses as its pivot
//
// NB the radius: the vegetation wind modes take their height envelope from the
// mesh AABB, which for one merged field spans the lot. Widen it enough that the
// ground dome falls appreciably across it and that envelope starts measuring
// terrain elevation instead of blade height, and grass in the hollows stops
// moving. Keep the field small enough that the ground under it is ~flat.
typedef struct GrassParams {
    int seed;
    float radius;       // extent of the field (see the note above)
    float clear_radius; // keep-out around the trunk; grass avoids wood
    float density;      // tufts per 100 square units inside a patch
    float patchiness;   // 0 = even coverage, 1 = islands of grass on bare earth
    float height;
    float blade_width;
    float bend;             // how far a blade arcs over; straight blades read as spikes
    float flower_amount;    // 0..1
    float seed_head_amount; // 0..1
} GrassParams;

// Build the field into `mesh`. False if the parameters produced nothing, in
// which case the mesh is untouched and the caller should discard it.
bool grass_build_mesh(const GrassParams* p, Mesh* mesh);

#endif // _GRASS_H_
