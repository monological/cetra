#ifndef _FIRE_INTERNAL_H_
#define _FIRE_INTERNAL_H_

#include "fire.h"

/*
 * What fire.c computes for the GPU half (fire_sim.c, fire_render.c) and nobody else: no GL here
 * either, and nothing an app reaches for.
 */

// How many of a GRID fire's obstacles and a FLIPBOOK's cards are read: the counts, which are
// plain stores, held to the arrays they count.
static inline int fire_obstacle_count(const Fire* fire) {
    const int n = fire->grid.obstacle_count;
    return n < 0 ? 0 : (n > FIRE_MAX_OBSTACLES ? FIRE_MAX_OBSTACLES : n);
}

static inline int fire_card_count(const Fire* fire) {
    const int n = fire->cards.count;
    return n < 0 ? 0 : (n > FIRE_MAX_CARDS ? FIRE_MAX_CARDS : n);
}

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

// What a length of gas emits, nits per metre into `rgb`, and its luminance: fire_emission.glsl's
// fireEmission, in the same arithmetic -- the blackbody at ambient + `rise` times the soot's
// absorption, plus the core's blue, through the fire's adaptation, clamped there, scaled by its
// brightness.
float fire_emission(const Fire* fire, float rise, float soot, float core, vec3 rgb);

// fire_grid.glsl's fireFadeAtEdge as a factor: how much of a GRID fire's soot and core at `p`,
// in cells, is drawn and cast, falling to 0 over the last FIRE_EDGE_FADE_CELLS before an open
// face of its `cells` box.
float fire_edge_fade(const Fire* fire, const int cells[3], const vec3 p);

// The cooling law's coefficient, 1 / (s K^3): Nguyen's c_T over the peak's rise to the fourth,
// what the simulation cools by and the light's sum counts the heat it sheds by.
float fire_cooling_rate(const FireParams* params);

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
