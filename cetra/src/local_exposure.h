#ifndef _LOCAL_EXPOSURE_H_
#define _LOCAL_EXPOSURE_H_

#include <GL/glew.h>

#include "uniform.h"

/*
 * Local exposure (spec 13.19, after Unreal 5's bilateral Local Exposure), owned by PostFX: an
 * exposure per pixel on top of the camera's, so a window onto a bright day comes down without the
 * room around it going dark. Durand & Dorsey's method -- the frame's log luminance split into an
 * edge-aware BASE and the DETAIL above it, only the base's contrast reduced -- on Chen, Paris &
 * Durand's bilateral grid, which a GL 4.1 fragment pass can build by gather and the tonemap can
 * slice in two taps.
 *
 * This module builds the grid and the blurred luminance from the frame each frame; the tonemap
 * reads them and applies the factor (shaders/include/local_exposure.glsl).
 */

typedef struct LocalExposure LocalExposure;

// Compiles the passes. NULL on failure, reported, which leaves the frame without local exposure.
LocalExposure* create_local_exposure(void);
void free_local_exposure(LocalExposure* le);

/*
 * Build this frame's grid and blurred luminance from `hdr_tex`, `frame_w` by `frame_h` and
 * exposed. `middle_grey` is the log2 luminance the camera maps the frame's mean to, which the
 * grid's bins are centred on; `kernel` is the blurred luminance's kernel as a fraction of the
 * frame's width. Returns the atlas the tonemap samples, or 0 when the targets could not be made,
 * which is reported and which a later frame of the same size would not change.
 */
GLuint local_exposure_run(LocalExposure* le, GLuint hdr_tex, int frame_w, int frame_h,
                          float middle_grey, float kernel, GLuint quad_vao);

// The atlas's layout, onto the program that slices it. Valid once local_exposure_run has run.
void local_exposure_upload_layout(const LocalExposure* le, UniformManager* u);

#endif
