#ifndef _GROUND_H_
#define _GROUND_H_

// The ground is a very shallow dome, wide enough to reach the horizon: at a
// small radius it reads as a saucer floating in the sky's virtual ground.
#define GROUND_RADIUS 900.0f
#define GROUND_HEIGHT 20.0f

// Surface height at a world XZ. Anything that sits ON the ground -- grass,
// scattered detail -- must place itself with this rather than at y = 0, and
// must use the same function the ground mesh is built from so the two cannot
// drift apart. Flat (0) beyond the rim.
float ground_height_at(float x, float z);

#endif // _GROUND_H_
