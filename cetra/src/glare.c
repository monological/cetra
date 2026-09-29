#include "glare.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ext/log.h"
#include "program.h"
#include "texture.h"
#include "util.h"

/*
 * The aperture, sampled at GLARE_PSF_RES a side with GLARE_PSF_SS^2 subsamples a texel: its
 * radius a ninth of the side, so its spectrum -- which is what is kept -- has room around it.
 * Clearwater's shape throughout, every number below theirs.
 */
#define GLARE_PSF_RES    512
#define GLARE_PSF_SS     3
#define GLARE_PSF_RADIUS (GLARE_PSF_RES * 0.11f)
// The grid the frame is transformed in, long side and short, and how much of it the frame
// takes: the rest is room for a spike to fade before the transform wraps it round.
#define GLARE_GRID_LONG  512
#define GLARE_GRID_SHORT 256
#define GLARE_FRAME_FILL 0.75f
// The pattern's full width over the frame's height.
#define GLARE_PSF_SPAN 1.15f
// Where the pattern's fade to its edge begins, as a fraction of its half-width; it reaches zero
// at the half-width, the largest circle the square pattern holds.
#define GLARE_PSF_FADE 0.6f
// What the source is stored at through the transform and restored from after it, so a glint of
// thousands keeps its fraction bits through the transform's many additions.
#define GLARE_SOURCE_SCALE 1.0e-3f

struct Glare {
    float* psf; // GLARE_PSF_RES^2 RGB, relative within each channel
    ShaderProgram *source, *fft, *multiply, *output;
    int frame_w, frame_h;    // what the grid and kernel were built for; 0 = not yet
    int grid_w, grid_h;      // the transform's grid
    int fill_w, fill_h;      // the frame's corner of it
    GLuint tex[2], fbo[2];   // red + i green, and blue, two complex signals a texel, ping-pong
    GLuint kernel;           // each channel's pattern spectrum on the grid, real, in .rgb
    GLuint out_tex, out_fbo; // the glare, the frame's shape
};

/*
 * Clearwater's generator for the dust, mulberry32: the specks have to land where theirs do, and
 * a different generator puts them elsewhere.
 */
static float _glare_mulberry(uint32_t* state) {
    *state += 0x6D2B79F5u;
    uint32_t t = (*state ^ (*state >> 15)) * (1u | *state);
    t = (t + (t ^ (t >> 7)) * (61u | t)) ^ t;
    return (float)((t ^ (t >> 14)) / 4294967296.0);
}

// In-place radix-2 FFT of one row, forward (sign -1) or unnormalised inverse (+1).
static void _glare_fft1(float* re, float* im, int n, int sign) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double angle = 2.0 * M_PI / len * sign;
        const float wr = (float)cos(angle), wi = (float)sin(angle);
        const int half = len >> 1;
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < half; k++) {
                const int a = i + k, b = a + half;
                const float xr = re[b] * cr - im[b] * ci;
                const float xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
                const float t = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = t;
            }
        }
    }
}

// Rows then columns. Returns false only when it cannot allocate a column.
static bool _glare_fft2(float* re, float* im, int w, int h, int sign) {
    for (int y = 0; y < h; y++)
        _glare_fft1(re + (size_t)y * w, im + (size_t)y * w, w, sign);
    float* cr = malloc(sizeof(float) * (size_t)h * 2);
    if (!cr)
        return false;
    float* ci = cr + h;
    for (int x = 0; x < w; x++) {
        for (int y = 0; y < h; y++) {
            cr[y] = re[(size_t)y * w + x];
            ci[y] = im[(size_t)y * w + x];
        }
        _glare_fft1(cr, ci, h, sign);
        for (int y = 0; y < h; y++) {
            re[(size_t)y * w + x] = cr[y];
            im[(size_t)y * w + x] = ci[y];
        }
    }
    free(cr);
    return true;
}

/*
 * The aperture's diffraction pattern, as the eye sees it: the power spectrum of its shape, then
 * that spectrum at eight wavelengths -- it scales with the wavelength -- each in its own colour.
 * Unnormalised: it is resampled to the grid before use, and normalised there.
 *
 * NOT lifted in the far field, where Clearwater multiplies it by up to 8 to imitate a phone lens:
 * that is no part of an aperture's diffraction, and over a glittering sea it summed into a veil
 * and long streaks (spec 13.5).
 *
 * Its central peak is kept: the star is the WHOLE image a point of light makes through this
 * aperture, core and spikes, and the tonemap moves the source's light into it.
 */
static float* _glare_build_psf(void) {
    const int n = GLARE_PSF_RES;
    const float r = GLARE_PSF_RADIUS;
    const float deg = (float)M_PI / 180.0f;
    float* re = calloc((size_t)n * n * 2, sizeof(float));
    float* power = malloc(sizeof(float) * (size_t)n * n);
    float* out = calloc((size_t)n * n * 3, sizeof(float));
    if (!re || !power || !out) {
        free(re);
        free(power);
        free(out);
        return NULL;
    }
    float* im = re + (size_t)n * n;

    // A round aperture with six slightly flattened sides, two hairline scratches and three
    // fainter, and seven specks of dust.
    float flats[6];
    for (int k = 0; k < 6; k++)
        flats[k] = (15.0f + (float)k * 60.0f) * deg;
    const float scratch[5][3] = {{21.0f, 0.12f, 2.2f},
                                 {22.5f, -0.38f, 1.6f},
                                 {19.0f, 0.55f, 1.2f},
                                 {152.0f, 0.25f, 1.0f},
                                 {84.0f, -0.2f, 0.8f}};
    float dust[7][3];
    uint32_t seed = 3u;
    for (int d = 0; d < 7; d++) {
        dust[d][0] = (_glare_mulberry(&seed) - 0.5f) * 1.4f * r;
        dust[d][1] = (_glare_mulberry(&seed) - 0.5f) * 1.4f * r;
        dust[d][2] = (0.015f + 0.03f * _glare_mulberry(&seed)) * r;
    }
    const int ss = GLARE_PSF_SS;
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            int open = 0;
            for (int sy = 0; sy < ss; sy++) {
                for (int sx = 0; sx < ss; sx++) {
                    const float dx = (float)x - n / 2 + ((float)sx + 0.5f) / ss - 0.5f;
                    const float dy = (float)y - n / 2 + ((float)sy + 0.5f) / ss - 0.5f;
                    if (dx * dx + dy * dy > r * r)
                        continue;
                    bool ok = true;
                    for (int k = 0; k < 6 && ok; k++)
                        ok = dx * cosf(flats[k]) + dy * sinf(flats[k]) <= r * 0.955f;
                    for (int k = 0; k < 5 && ok; k++) {
                        const float a = scratch[k][0] * deg;
                        ok = fabsf(dx * cosf(a) + dy * sinf(a) - scratch[k][1] * r) >=
                             scratch[k][2] * 0.5f;
                    }
                    for (int d = 0; d < 7 && ok; d++) {
                        const float ex = dx - dust[d][0], ey = dy - dust[d][1];
                        ok = ex * ex + ey * ey >= dust[d][2] * dust[d][2];
                    }
                    open += ok;
                }
            }
            re[(size_t)y * n + x] = (float)open / (float)(ss * ss);
        }
    }
    if (!_glare_fft2(re, im, n, n, -1)) {
        free(re);
        free(power);
        free(out);
        return NULL;
    }
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            const size_t i = (size_t)((y + n / 2) % n) * n + (size_t)((x + n / 2) % n);
            power[(size_t)y * n + x] = re[i] * re[i] + im[i] * im[i];
        }
    }
    free(re);

    // The pattern's size goes as the wavelength; eight bands across the visible, each in the
    // colour Clearwater gives it.
    const float bands[8][4] = {{440.0f, 0.10f, 0.00f, 0.85f}, {470.0f, 0.00f, 0.15f, 1.00f},
                               {500.0f, 0.00f, 0.60f, 0.55f}, {530.0f, 0.05f, 1.00f, 0.15f},
                               {560.0f, 0.45f, 0.95f, 0.00f}, {590.0f, 0.95f, 0.55f, 0.00f},
                               {620.0f, 1.00f, 0.20f, 0.00f}, {650.0f, 0.70f, 0.05f, 0.00f}};
    for (int b = 0; b < 8; b++) {
        const float s = bands[b][0] / 550.0f;
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) {
                const float u = n / 2 + ((float)x - n / 2) / s;
                const float v = n / 2 + ((float)y - n / 2) / s;
                if (u < 0.0f || v < 0.0f || u >= n - 1 || v >= n - 1)
                    continue;
                const int x0 = (int)u, y0 = (int)v;
                const float fx = u - x0, fy = v - y0;
                const float* p = power + (size_t)y0 * n + x0;
                const float val = ((p[0] * (1 - fx) + p[1] * fx) * (1 - fy) +
                                   (p[n] * (1 - fx) + p[n + 1] * fx) * fy) /
                                  (s * s);
                float* o = out + ((size_t)y * n + x) * 3;
                for (int c = 0; c < 3; c++)
                    o[c] += val * bands[b][1 + c];
            }
        }
    }
    free(power);

    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            const float rr = hypotf((float)x - n / 2, (float)y - n / 2);
            // Faded to nothing inside the inscribed circle, so a spike trails off rather than
            // stopping at the pattern's square edge -- which it does visibly round a source bright
            // enough that the spikes are still lit where the stored pattern ends.
            const float t =
                fminf(1.0f, fmaxf(0.0f, (rr / (n / 2) - GLARE_PSF_FADE) / (1.0f - GLARE_PSF_FADE)));
            const float fade = 1.0f - t * t * (3.0f - 2.0f * t);
            float* o = out + ((size_t)y * n + x) * 3;
            for (int c = 0; c < 3; c++)
                o[c] *= fade;
        }
    }
    return out;
}

Glare* create_glare(void) {
    Glare* g = calloc(1, sizeof(Glare));
    if (!g)
        return NULL;
    g->psf = _glare_build_psf();
    g->source = create_glare_source_program();
    g->fft = create_glare_fft_program();
    g->multiply = create_glare_multiply_program();
    g->output = create_glare_output_program();
    if (!g->psf || !g->source || !g->fft || !g->multiply || !g->output) {
        log_error("Glare: aperture pattern or programs unavailable");
        free_glare(g);
        return NULL;
    }
    return g;
}

static void _glare_free_targets(Glare* g) {
    for (int i = 0; i < 2; i++) {
        gl_delete_texture(&g->tex[i]);
        gl_delete_fbo(&g->fbo[i]);
    }
    gl_delete_texture(&g->kernel);
    gl_delete_texture(&g->out_tex);
    gl_delete_fbo(&g->out_fbo);
    g->frame_w = g->frame_h = 0;
}

void free_glare(Glare* g) {
    if (!g)
        return;
    _glare_free_targets(g);
    free_program(g->source);
    free_program(g->fft);
    free_program(g->multiply);
    free_program(g->output);
    free(g->psf);
    free(g);
}

static bool _glare_target(int w, int h, GLenum format, GLuint* tex, GLuint* fbo) {
    *tex = create_texture_2d_float(w, h, format, GL_RGBA, NULL);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

/*
 * The grid and the pattern's spectrum on it, for a frame of this shape: the frame shrunk into
 * GLARE_FRAME_FILL of the grid, and the pattern resampled so its full width spans GLARE_PSF_SPAN
 * of the frame's height, centred on the grid's origin as a convolution kernel wants, normalised
 * per channel so it carries the light it is handed once, and divided by the grid's size so the
 * inverse transform needs no normalising of its own. Only the real part of its transform is kept,
 * which is its even part: the pattern is symmetric about its centre but for the resample's last
 * edge row, and that is all it drops.
 */
static bool _glare_build_targets(Glare* g, int frame_w, int frame_h) {
    _glare_free_targets(g);
    g->grid_w = frame_w >= frame_h ? GLARE_GRID_LONG : GLARE_GRID_SHORT;
    g->grid_h = frame_w >= frame_h ? GLARE_GRID_SHORT : GLARE_GRID_LONG;
    const float fit =
        fminf(g->grid_w * GLARE_FRAME_FILL / frame_w, g->grid_h * GLARE_FRAME_FILL / frame_h);
    g->fill_w = (int)fmaxf(1.0f, roundf(frame_w * fit));
    g->fill_h = (int)fmaxf(1.0f, roundf(frame_h * fit));
    const int gw = g->grid_w, gh = g->grid_h;
    const size_t cells = (size_t)gw * gh;

    float* k = calloc(cells * 6, sizeof(float)); // re and im for each of three channels
    float* packed = calloc(cells * 4, sizeof(float));
    if (!k || !packed) {
        free(k);
        free(packed);
        return false;
    }
    const int n = GLARE_PSF_RES;
    const float scale = n / (GLARE_PSF_SPAN * g->fill_h);
    double total[3] = {0.0, 0.0, 0.0};
    for (int gy = -gh / 2; gy < gh / 2; gy++) {
        for (int gx = -gw / 2; gx < gw / 2; gx++) {
            const float u = n / 2 + gx * scale, v = n / 2 + gy * scale;
            if (u < 0.0f || v < 0.0f || u >= n - 1 || v >= n - 1)
                continue;
            const int x0 = (int)u, y0 = (int)v;
            const float fx = u - x0, fy = v - y0;
            const size_t i = (size_t)((gy + gh) % gh) * gw + (size_t)((gx + gw) % gw);
            for (int c = 0; c < 3; c++) {
                const float* p = g->psf + ((size_t)y0 * n + x0) * 3 + c;
                const float val = (p[0] * (1 - fx) + p[3] * fx) * (1 - fy) +
                                  (p[n * 3] * (1 - fx) + p[n * 3 + 3] * fx) * fy;
                k[c * 2 * cells + i] = val;
                total[c] += val;
            }
        }
    }
    bool ok = true;
    for (int c = 0; c < 3 && ok; c++) {
        float* re = k + c * 2 * cells;
        const float norm = (float)(1.0 / ((double)cells * fmax(total[c], 1e-30)));
        for (size_t i = 0; i < cells; i++)
            re[i] *= norm;
        ok = _glare_fft2(re, re + cells, gw, gh, -1);
    }
    for (size_t i = 0; i < cells && ok; i++)
        for (int c = 0; c < 3; c++)
            packed[i * 4 + c] = k[c * 2 * cells + i];
    free(k);
    if (!ok) {
        free(packed);
        return false;
    }
    g->kernel = create_texture_2d_float(gw, gh, GL_RGBA32F, GL_RGBA, packed);
    free(packed);

    for (int i = 0; i < 2 && ok; i++)
        ok = _glare_target(gw, gh, GL_RGBA32F, &g->tex[i], &g->fbo[i]);
    if (ok) {
        ok = _glare_target(g->fill_w, g->fill_h, GL_RGBA16F, &g->out_tex, &g->out_fbo);
        // Read across the whole frame by the tonemap, so it filters.
        glBindTexture(GL_TEXTURE_2D, g->out_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    if (!ok)
        return false;
    g->frame_w = frame_w;
    g->frame_h = frame_h;
    return true;
}

/*
 * One whole 2D transform of the ping-pong pair, starting from `from`: every stage along x, then
 * every stage along y. Returns which of the pair holds the result.
 */
static int _glare_fft(Glare* g, int from, float sign, GLuint quad) {
    glUseProgram(g->fft->id);
    UniformManager* u = g->fft->uniforms;
    uniform_set_int(u, "glareSrc", 0);
    uniform_set_float(u, "glareSign", sign);
    glViewport(0, 0, g->grid_w, g->grid_h);
    int src = from;
    for (int axis = 0; axis < 2; axis++) {
        const int len = axis == 0 ? g->grid_w : g->grid_h;
        uniform_set_int(u, "glareHoriz", axis == 0 ? 1 : 0);
        uniform_set_int(u, "glareHalf", len / 2);
        for (int span = 1; span < len; span <<= 1) {
            glBindFramebuffer(GL_FRAMEBUFFER, g->fbo[1 - src]);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g->tex[src]);
            uniform_set_int(u, "glareSpan", span);
            draw_fullscreen_quad(quad);
            src = 1 - src;
        }
    }
    return src;
}

// The first three channels of a `w`-wide texture summed over its [0, x1) x [0, y1) corner.
static void _glare_sum(GLuint tex, int w, int h, int x1, int y1, double* out3) {
    float* px = malloc(sizeof(float) * (size_t)w * h * 4);
    if (!px)
        return;
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, px);
    for (int y = 0; y < y1; y++)
        for (int x = 0; x < x1; x++)
            for (int c = 0; c < 3; c++)
                out3[c] += px[((size_t)y * w + x) * 4 + c];
    free(px);
}

GLuint glare_run(Glare* g, GLuint hdr_tex, int frame_w, int frame_h, float threshold, GLuint quad,
                 bool probe) {
    if (!g || frame_w <= 0 || frame_h <= 0)
        return 0;
    if ((frame_w != g->frame_w || frame_h != g->frame_h) &&
        !_glare_build_targets(g, frame_w, frame_h)) {
        log_error("Glare: grid targets unavailable");
        return 0;
    }
    glDisable(GL_BLEND);

    // The bright light, into the frame's corner of the grid, the rest cleared.
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo[0]);
    glViewport(0, 0, g->grid_w, g->grid_h);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, g->fill_w, g->fill_h);
    glUseProgram(g->source->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hdr_tex);
    uniform_set_int(g->source->uniforms, "hdrTex", 0);
    uniform_set_float(g->source->uniforms, "glareThreshold", threshold);
    uniform_set_float(g->source->uniforms, "glareSourceScale", GLARE_SOURCE_SCALE);
    draw_fullscreen_quad(quad);
    double source[3] = {0.0, 0.0, 0.0};
    if (probe)
        _glare_sum(g->tex[0], g->grid_w, g->grid_h, g->fill_w, g->fill_h, source);

    // Forward, times the pattern's spectrum, back.
    const int forward = _glare_fft(g, 0, -1.0f, quad);
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo[1 - forward]);
    glUseProgram(g->multiply->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->tex[forward]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g->kernel);
    uniform_set_int(g->multiply->uniforms, "glareSrc", 0);
    uniform_set_int(g->multiply->uniforms, "glareKernel", 1);
    draw_fullscreen_quad(quad);
    const int result = _glare_fft(g, 1 - forward, 1.0f, quad);

    glBindFramebuffer(GL_FRAMEBUFFER, g->out_fbo);
    glViewport(0, 0, g->fill_w, g->fill_h);
    glUseProgram(g->output->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->tex[result]);
    uniform_set_int(g->output->uniforms, "glareResult", 0);
    uniform_set_float(g->output->uniforms, "glareSourceScale", GLARE_SOURCE_SCALE);
    draw_fullscreen_quad(quad);

    if (probe) {
        double glare[3] = {0.0, 0.0, 0.0};
        _glare_sum(g->out_tex, g->fill_w, g->fill_h, g->fill_w, g->fill_h, glare);
        // The source as stored, the glare restored to full scale.
        printf("glare-probe source=%.6g,%.6g,%.6g glare=%.6g,%.6g,%.6g\n",
               source[0] / GLARE_SOURCE_SCALE, source[1] / GLARE_SOURCE_SCALE,
               source[2] / GLARE_SOURCE_SCALE, glare[0], glare[1], glare[2]);
    }
    check_gl_error("glare");
    return g->out_tex;
}
