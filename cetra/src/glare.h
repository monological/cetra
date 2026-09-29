#ifndef _GLARE_H_
#define _GLARE_H_

#include <GL/glew.h>
#include <stdbool.h>

/*
 * Diffraction glare (spec 13.4, after Clearwater): the star a camera's aperture draws round
 * every point of light too bright for it, owned by PostFX.
 *
 * The star is the aperture's diffraction pattern -- the power spectrum of its shape, a round
 * lens with slightly flattened sides, a few hairline scratches and some dust -- summed over the
 * visible spectrum, so its spikes carry faint colour. The frame's brightest light is convolved
 * with it by FFT, every frame, so its cost does not depend on how many glints there are.
 *
 * The pattern is built once, on the CPU; its spectrum is resampled to the grid whenever the
 * frame's shape changes. The grid is 512 by 256 (256 by 512 for a portrait frame) with the frame
 * shrunk into three quarters of it, so a spike has room to fade before it wraps round.
 */

typedef struct Glare Glare;

// Builds the aperture's pattern. NULL on failure, reported with its reason, which leaves the
// frame without glare.
Glare* create_glare(void);
void free_glare(Glare* glare);

/*
 * Star everything in `hdr_tex` brighter than `threshold` (working space, the same units the
 * tonemap reads), for a frame `frame_w` by `frame_h`. Returns the glare image to add, which
 * covers the whole frame, or 0 when the pass could not run -- its targets could not be made, which
 * is reported and which a later frame of the same shape would not change. `probe` prints the light
 * the source held against the light the glare carries, per channel -- the pattern is normalised, so
 * the two differ only by what spills past the frame's edge -- and the mean light per pixel the
 * tonemap takes out of the frame against what the glare puts back, at strength 1.
 */
GLuint glare_run(Glare* glare, GLuint hdr_tex, int frame_w, int frame_h, float threshold,
                 GLuint quad_vao, bool probe);

#endif
