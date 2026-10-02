#ifndef _FIRE_INTERNAL_H_
#define _FIRE_INTERNAL_H_

#include "fire.h"

/*
 * What fire.c computes for the GPU half (fire_sim.c, fire_render.c) and nobody else: no GL here
 * either, and nothing an app reaches for.
 */

// A GRID fire's cells along each axis, from its size and cell and capped at FIRE_GRID_MAX_*;
// the cell size taken; and its box, local to the fire's origin: those cells about `center`.
void fire_grid_cells(const Fire* fire, int cells[3]);
float fire_grid_cell(const Fire* fire);
void fire_grid_bounds(const Fire* fire, vec3 min, vec3 max);

// Where `card` is in its flipbook's loop at time `t`, in frames, 0..frames: what it casts and
// what it draws are both read there.
double fire_card_frame(const FireFlipbook* book, const FireCard* card, double t);
// A card's width and height: its own, or the size the sheet's frames were made at.
void fire_card_size(const Fire* fire, const FireCard* card, vec2 out);

// The blackbody table the GPU is handed, FIRE_BB_LUT_SIZE RGBA texels: the Rec.709
// chromaticity (rgb over luminance) and log10 of the luminance, over FIRE_BB_T_MIN..MAX K.
const float* fire_blackbody_table(void);
// A blackbody at `kelvin` read from that table as the shader reads it: nits per channel into
// `rgb`, UNCLAMPED (blue goes negative below about 1900 K), and the luminance returned. Black
// below FIRE_BB_T_MIN.
float fire_blackbody(float kelvin, vec3 rgb);
// The blue core's colour, its band emission through the observer: Rec.709, luminance 1,
// unclamped like the blackbody.
const float* fire_blue_color(void);

// The two texels a fire's light sums to, as the GPU writes them and the CPU reads them back:
// (intensity, its first moments in the box's local frame) and (emitted rgb, heat release).
typedef struct FireAnswer {
    float intensity;
    vec3 centroid; // local to the fire's origin; the box's centre when nothing burns
    vec3 color;    // luminance 1; white when nothing burns
    float heat_release;
} FireAnswer;
FireAnswer fire_answer_decode(const float texels[8], const vec3 fallback_centroid);

#endif // _FIRE_INTERNAL_H_
