#ifndef _WATER_WAVES_H_
#define _WATER_WAVES_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "../water.h"

/*
 * The Gerstner wave train, on the CPU.
 *
 * This is the same sum ocean.glsl evaluates, in the same order, from the same Water
 * fields -- so a query here and the surface a frame rasterizes agree. It exists because
 * nothing outside a shader can ask a texture where the surface is: buoyancy, a boat
 * hull, a splash spawn point and a gameplay "am I underwater" test all need the answer
 * on the CPU, at one point, now.
 *
 * WHY IT LIVES IN procedural/ RATHER THAN water.c. It is a pure function of the wave
 * parameters and a position, with no GL state and no subsystem -- the same shape as
 * terrain.h's height query, and consumed the same way. water.c owns GPU resources and a
 * render pass; this owns neither.
 *
 * THE SPECTRAL PATH IS NOT ANSWERED HERE, and cannot be. An FFT cascade's displacement
 * exists only as a texture the transform just wrote, so reproducing it on the CPU means
 * running the transform on the CPU -- a second implementation of the thing whose whole
 * point is that it runs on the GPU. Callers get the still level for that model, and
 * water_waves_available says so rather than leaving them to discover a flat answer.
 */

// Whether water_height_at returns a displaced surface or just the still level.
bool water_waves_available(const Water* water);

/*
 * Surface height under (x, z) at time t, in world units.
 *
 * `t` is the same clock the render passes ocean.glsl -- engine->render_time -- and
 * passing a different one silently answers about a different instant of the same sea.
 *
 * Returns water->level unchanged for a NULL or disabled surface, and for the spectral
 * model (see above), so a caller that forgot to check gets a usable plane rather than a
 * zero.
 */
float water_height_at(const Water* water, float x, float z, float t);

/*
 * Height and the surface normal together, for anything that needs an orientation --
 * a hull to sit level, a splash to eject along the surface.
 *
 * One call rather than two because the normal comes out of the same derivatives the
 * height sum already computes: asking separately would evaluate the whole train twice
 * and, worse, invites the two answers to come from different `t`.
 */
float water_surface_at(const Water* water, float x, float z, float t, vec3 out_normal);

/*
 * How far the recovered parameter lands from the position that was asked about, in world
 * units. 0 for a still or spectral surface, where there is no map to invert.
 *
 * A diagnostic, and the only externally visible check on the solver: everything else
 * this file returns looks plausible whether or not the inversion converged, because an
 * unconverged answer is still a point on the surface -- just not the one over the query.
 */
float water_waves_inverse_residual(const Water* water, float x, float z, float t);

#endif // _WATER_WAVES_H_
