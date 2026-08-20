#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "erosion.h"

#include "../thread.h"
#include "../util.h"

// The largest worker count this bake will use. Matches the cloud noise bake's
// ceiling and for the same reason: the slab arrays are stack-allocated, so the
// bound has to be a constant, and past eight the stages are memory-bound anyway.
#define EROSION_MAX_WORKERS 8

ErosionParams erosion_default_params(void) {
    ErosionParams p;
    p.iterations = 220;
    p.dt = 0.02f;
    // RAIN OVER EVAPORATION IS THE EQUILIBRIUM WATER DEPTH, and that is the one
    // number to get right before any of the others mean anything. Uniform rain
    // against proportional evaporation settles at rain/evaporation everywhere, so
    // the first draft's 0.55 over 0.055 put TEN UNITS of standing water over a
    // terrain whose cells are two units across -- an ocean, not a catchment. Every
    // cell then drained into every other, flow came out uniform, and the mask
    // painted the whole map one colour. 0.02 over 0.5 is 40 mm, a wet hillside.
    p.rain = 0.02f;
    p.evaporation = 0.5f;
    p.capacity = 0.32f;
    p.dissolve = 0.45f;
    p.deposit = 0.45f;
    // A degree and a half. Small enough that a valley floor still carries its
    // load downstream, large enough that standing water does not dump everything
    // the moment velocity crosses zero.
    p.min_tilt = 0.026f;
    p.talus = 0.62f;
    p.thermal_rate = 0.30f;
    p.thermal_every = 4;
    p.workers = 0;
    return p;
}

// Every plane the sim needs, all res*res floats. Grouped in one struct so a stage
// takes one pointer and the worker payload stays a band index rather than a
// growing argument list.
typedef struct Planes {
    int res;
    float cell;    // world units between adjacent cells
    float inv_cell;

    float* h; // ground height, the thing being eroded
    float* w; // standing water depth
    float* w_prev; // depth the fluxes were computed against, for transport
    float* s;      // suspended sediment
    float* s_next;

    float* fl; // outflow to the four neighbours, in volume per second
    float* fr;
    float* ft;
    float* fb;

    float* vx; // flow velocity
    float* vz;

    float* scratch; // per-stage intermediate: capacity, then thermal deltas

    float* flow_acc; // mask accumulators
    float* dep_acc;
    float* wear_acc;

    ErosionParams p;
} Planes;

typedef struct Band {
    const Planes* pl;
    int j0;
    int j1;
} Band;

// Stages are written as one of these and run over disjoint row bands.
typedef void (*StageFn)(const Planes*, int j0, int j1);

typedef struct BandJob {
    Band band;
    StageFn fn;
} BandJob;

static void* band_worker(void* arg) {
    BandJob* job = (BandJob*)arg;
    job->fn(job->band.pl, job->band.j0, job->band.j1);
    return NULL;
}

// Run one stage across worker threads and join before returning.
//
// The join between stages IS the double buffer's enforcement: within a stage no
// cell reads a value another cell writes, and across stages every read is of a
// plane the previous stage finished. Remove the join and the sim still produces
// eroded-looking terrain, which is why the gate asserts thread counts against each
// other rather than trusting the output to look wrong.
static void run_stage(const Planes* pl, StageFn fn, int workers) {
    if (workers < 2) {
        fn(pl, 0, pl->res);
        return;
    }

    BandJob jobs[EROSION_MAX_WORKERS];
    cetra_thread_t threads[EROSION_MAX_WORKERS];
    bool running[EROSION_MAX_WORKERS] = {false};

    for (int i = 0; i < workers; i++) {
        // Remainder-free split, the cloud bake's form: never overshoots and every
        // worker gets a band whatever res and workers are.
        jobs[i].band.pl = pl;
        jobs[i].band.j0 = pl->res * i / workers;
        jobs[i].band.j1 = pl->res * (i + 1) / workers;
        jobs[i].fn = fn;
        running[i] = cetra_thread_create(&threads[i], band_worker, &jobs[i]);
        if (!running[i])
            band_worker(&jobs[i]); // could not start: run the band inline
    }
    for (int i = 0; i < workers; i++) {
        if (running[i])
            cetra_thread_join(threads[i]);
    }
}

static inline size_t idx(const Planes* pl, int i, int j) {
    return (size_t)j * (size_t)pl->res + (size_t)i;
}

// Edge cells have no neighbour outside the grid and no flux across that face, so
// the domain is a closed basin: water and sediment stay in. That is what lets the
// gate assert a sediment budget at all -- with open edges the budget leaks by an
// amount only the sim knows.
static inline bool inside(const Planes* pl, int i, int j) {
    return i >= 0 && j >= 0 && i < pl->res && j < pl->res;
}

static void stage_rain(const Planes* pl, int j0, int j1) {
    float add = pl->p.rain * pl->p.dt;
    for (int j = j0; j < j1; j++) {
        for (int i = 0; i < pl->res; i++)
            pl->w[idx(pl, i, j)] += add;
    }
}

// Outflow through four virtual pipes, driven by the difference in water SURFACE
// height. Reads h and w, which nothing in this stage writes, and writes only its
// own four flux values -- so no cell can observe another's output and in-place is
// safe here.
static void stage_flux(const Planes* pl, int j0, int j1) {
    const float dt = pl->p.dt;
    // Pipe cross-section over length, folded with gravity into one constant.
    const float accel = dt * 9.81f * pl->inv_cell;
    const float cell_area = pl->cell * pl->cell;

    for (int j = j0; j < j1; j++) {
        for (int i = 0; i < pl->res; i++) {
            size_t c = idx(pl, i, j);
            float surface = pl->h[c] + pl->w[c];

            float f[4] = {pl->fl[c], pl->fr[c], pl->ft[c], pl->fb[c]};
            const int nx[4] = {i - 1, i + 1, i, i};
            const int nz[4] = {j, j, j - 1, j + 1};

            float total = 0.0f;
            for (int k = 0; k < 4; k++) {
                if (!inside(pl, nx[k], nz[k])) {
                    f[k] = 0.0f;
                    continue;
                }
                size_t n = idx(pl, nx[k], nz[k]);
                float drop = surface - (pl->h[n] + pl->w[n]);
                f[k] = f[k] + accel * drop;
                if (f[k] < 0.0f)
                    f[k] = 0.0f;
                total += f[k];
            }

            // Scale the four back so they cannot move more water than the cell
            // holds. This is the model's whole stability argument: without it a
            // steep cell overdraws, goes negative, and the next step overcorrects.
            if (total * dt > pl->w[c] * cell_area) {
                float k = (pl->w[c] * cell_area) / (total * dt);
                for (int m = 0; m < 4; m++)
                    f[m] *= k;
            }

            pl->fl[c] = f[0];
            pl->fr[c] = f[1];
            pl->ft[c] = f[2];
            pl->fb[c] = f[3];
        }
    }
}

// Apply the flux divergence to the water column and read a velocity out of it.
// Reads NEIGHBOURS' flux, which is why this cannot be fused with the stage above.
static void stage_water(const Planes* pl, int j0, int j1) {
    const float dt = pl->p.dt;
    const float inv_area = 1.0f / (pl->cell * pl->cell);

    for (int j = j0; j < j1; j++) {
        for (int i = 0; i < pl->res; i++) {
            size_t c = idx(pl, i, j);

            float in_l = inside(pl, i - 1, j) ? pl->fr[idx(pl, i - 1, j)] : 0.0f;
            float in_r = inside(pl, i + 1, j) ? pl->fl[idx(pl, i + 1, j)] : 0.0f;
            float in_t = inside(pl, i, j - 1) ? pl->fb[idx(pl, i, j - 1)] : 0.0f;
            float in_b = inside(pl, i, j + 1) ? pl->ft[idx(pl, i, j + 1)] : 0.0f;

            float out = pl->fl[c] + pl->fr[c] + pl->ft[c] + pl->fb[c];
            float net = (in_l + in_r + in_t + in_b) - out;

            float before = pl->w[c];
            // Kept because the transport stage moves sediment by the same fluxes,
            // and those were computed against the depth BEFORE this update.
            pl->w_prev[c] = before;
            float after = before + net * dt * inv_area;
            if (after < 0.0f)
                after = 0.0f;
            pl->w[c] = after;

            // Velocity from the mean flow through the cell, over the mean depth
            // across the step. Using the depth AFTER alone divides by a vanishing
            // number wherever the last of the water leaves, and the resulting
            // spike erodes a pit exactly where the sim should be doing nothing.
            float mean_depth = 0.5f * (before + after);
            float scale = mean_depth > 1e-5f ? 1.0f / (pl->cell * mean_depth) : 0.0f;
            pl->vx[c] = 0.5f * ((in_l - pl->fl[c]) + (pl->fr[c] - in_r)) * scale;
            pl->vz[c] = 0.5f * ((in_t - pl->ft[c]) + (pl->fb[c] - in_b)) * scale;
        }
    }
}

// Sediment capacity, into scratch. Split from the stage that applies it because
// capacity needs the tilt of the CURRENT ground, and applying erosion in place
// would let a cell read a neighbour's already-lowered height.
static void stage_capacity(const Planes* pl, int j0, int j1) {
    for (int j = j0; j < j1; j++) {
        for (int i = 0; i < pl->res; i++) {
            size_t c = idx(pl, i, j);

            int il = i > 0 ? i - 1 : i;
            int ir = i < pl->res - 1 ? i + 1 : i;
            int jt = j > 0 ? j - 1 : j;
            int jb = j < pl->res - 1 ? j + 1 : j;
            float dhx = (pl->h[idx(pl, ir, j)] - pl->h[idx(pl, il, j)]) * 0.5f * pl->inv_cell;
            float dhz = (pl->h[idx(pl, i, jb)] - pl->h[idx(pl, i, jt)]) * 0.5f * pl->inv_cell;

            // sin of the tilt, from the gradient, without forming the angle.
            float grad2 = dhx * dhx + dhz * dhz;
            float tilt = sqrtf(grad2 / (1.0f + grad2));
            if (tilt < pl->p.min_tilt)
                tilt = pl->p.min_tilt;

            float speed = sqrtf(pl->vx[c] * pl->vx[c] + pl->vz[c] * pl->vz[c]);
            // Depth-limited: a millimetre of water moving fast carries almost
            // nothing, and without this term ridges erode as hard as channels.
            float depth = pl->w[c];
            if (depth > 1.0f)
                depth = 1.0f;

            pl->scratch[c] = pl->p.capacity * tilt * speed * depth;
        }
    }
}

static void stage_erode(const Planes* pl, int j0, int j1) {
    for (int j = j0; j < j1; j++) {
        for (int i = 0; i < pl->res; i++) {
            size_t c = idx(pl, i, j);
            float cap = pl->scratch[c];
            float load = pl->s[c];

            if (cap > load) {
                float amount = pl->p.dissolve * (cap - load);
                pl->h[c] -= amount;
                pl->s[c] = load + amount;
                pl->wear_acc[c] += amount;
            } else {
                float amount = pl->p.deposit * (load - cap);
                pl->h[c] += amount;
                pl->s[c] = load - amount;
                pl->dep_acc[c] += amount;
            }

            // DRAINAGE, not depth and not speed. Rain falls on every cell, so
            // standing depth is high everywhere and paints the whole map one
            // colour -- measured, and it washed every trace of the network out.
            // Speed instead lights up every steep face, channel or not. What makes
            // a stream bed is how much water PASSED THROUGH, which is the outflow
            // volume, and it is the only one of the three that accumulates along a
            // drainage path the way a river does.
            pl->flow_acc[c] += (pl->fl[c] + pl->fr[c] + pl->ft[c] + pl->fb[c]) * pl->p.dt;
        }
    }
}

// Sediment rides the same pipes the water does, in the same proportion.
//
// A semi-Lagrangian gather was written first, because it is the textbook form,
// and it lost 3.06% of the sediment budget over 220 iterations: a bilinear gather
// conserves mass only for a divergence-free field and this one is emphatically
// not. Moving the load by the FLUXES instead makes every gram one cell gives away
// a gram exactly one neighbour receives, so the budget closes to rounding -- and
// the probe can assert closure rather than quote a tolerance nobody chose.
static void stage_transport(const Planes* pl, int j0, int j1) {
    const float dt = pl->p.dt;
    const float area = pl->cell * pl->cell;

    for (int j = j0; j < j1; j++) {
        for (int i = 0; i < pl->res; i++) {
            size_t c = idx(pl, i, j);
            float kept = pl->s[c];

            float held = pl->w_prev[c] * area;
            if (held > 1e-9f) {
                float out = (pl->fl[c] + pl->fr[c] + pl->ft[c] + pl->fb[c]) * dt / held;
                if (out > 1.0f)
                    out = 1.0f;
                kept -= pl->s[c] * out;
            }

            // What arrives, recomputed from each neighbour's own numbers rather
            // than from a shared pass. The duplication is the point: a scatter
            // would have to write the neighbour, which is the hazard that makes
            // the usual implementation disagree with itself under threads.
            const int nx[4] = {i - 1, i + 1, i, i};
            const int nz[4] = {j, j, j - 1, j + 1};
            for (int k = 0; k < 4; k++) {
                if (!inside(pl, nx[k], nz[k]))
                    continue;
                size_t n = idx(pl, nx[k], nz[k]);
                float n_held = pl->w_prev[n] * area;
                if (n_held <= 1e-9f)
                    continue;
                float n_out = (pl->fl[n] + pl->fr[n] + pl->ft[n] + pl->fb[n]) * dt / n_held;
                if (n_out <= 0.0f)
                    continue;
                // The same clamp the giving side applies, so the two halves agree
                // exactly rather than nearly.
                //
                // It is a ROUNDING GUARD and not a real branch: stage_flux already
                // scaled these four so that their total over dt cannot exceed the
                // water this cell held, and w_prev is that same depth, so n_out is
                // 1 by construction at worst. Only float rounding can put it over.
                // Kept because the two halves must agree BITWISE for the budget to
                // close -- if the giving side clamps on a rounding excursion and
                // this side does not, the difference is real material.
                float scale = n_out > 1.0f ? 1.0f / n_out : 1.0f;
                // The neighbour's pipe pointing AT this cell: its right pipe if it
                // sits to our left, and so on round.
                const float toward[4] = {pl->fr[n], pl->fl[n], pl->fb[n], pl->ft[n]};
                kept += pl->s[n] * (toward[k] * dt / n_held) * scale;
            }

            pl->s_next[c] = kept;
        }
    }
}

static void stage_evaporate(const Planes* pl, int j0, int j1) {
    float keep = 1.0f - pl->p.evaporation * pl->p.dt;
    if (keep < 0.0f)
        keep = 0.0f;
    for (int j = j0; j < j1; j++) {
        for (int i = 0; i < pl->res; i++)
            pl->w[idx(pl, i, j)] *= keep;
    }
}

// Thermal erosion, gather form: a cell works out how much it OWES its lower
// neighbours and how much it is owed by its higher ones, and settles both at once
// into scratch. Written as a gather so no cell writes another's height, which the
// scatter form of this algorithm does and which is what makes the usual
// implementation non-deterministic under threads.
static void stage_thermal(const Planes* pl, int j0, int j1) {
    const float talus_drop = pl->p.talus * pl->cell;
    const int nx[8] = {-1, 1, 0, 0, -1, 1, -1, 1};
    const int nz[8] = {0, 0, -1, 1, -1, -1, 1, 1};

    for (int j = j0; j < j1; j++) {
        for (int i = 0; i < pl->res; i++) {
            size_t c = idx(pl, i, j);
            float here = pl->h[c];
            float delta = 0.0f;

            // What this cell sheds. Diagonal neighbours are a longer run for the
            // same drop, so they get the diagonal threshold or a cliff comes out
            // with eight-fold symmetry stamped into it.
            float excess[8];
            float excess_total = 0.0f;
            for (int k = 0; k < 8; k++) {
                excess[k] = 0.0f;
                if (!inside(pl, i + nx[k], j + nz[k]))
                    continue;
                float run = (k < 4) ? talus_drop : talus_drop * 1.41421356f;
                float diff = here - pl->h[idx(pl, i + nx[k], j + nz[k])];
                if (diff > run) {
                    excess[k] = diff - run;
                    excess_total += excess[k];
                }
            }
            if (excess_total > 0.0f) {
                // Move a fraction of the single largest excess, split among the
                // neighbours in proportion. Moving the SUM would let one cell
                // shed more than its own relief in a step and invert the slope.
                float largest = 0.0f;
                for (int k = 0; k < 8; k++)
                    largest = excess[k] > largest ? excess[k] : largest;
                delta -= pl->p.thermal_rate * 0.5f * largest;
            }

            // What it receives, recomputed from each higher neighbour's point of
            // view. The arithmetic is duplicated on purpose: a shared pass would
            // have to write both cells, which is the hazard this form avoids.
            for (int k = 0; k < 8; k++) {
                int ni = i + nx[k], nj = j + nz[k];
                if (!inside(pl, ni, nj))
                    continue;
                float up = pl->h[idx(pl, ni, nj)];
                if (up - here <= 0.0f)
                    continue;

                float up_excess_total = 0.0f, up_largest = 0.0f, mine = 0.0f;
                for (int m = 0; m < 8; m++) {
                    int mi = ni + nx[m], mj = nj + nz[m];
                    if (!inside(pl, mi, mj))
                        continue;
                    float run = (m < 4) ? talus_drop : talus_drop * 1.41421356f;
                    float diff = up - pl->h[idx(pl, mi, mj)];
                    if (diff <= run)
                        continue;
                    float e = diff - run;
                    up_excess_total += e;
                    up_largest = e > up_largest ? e : up_largest;
                    if (mi == i && mj == j)
                        mine = e;
                }
                if (up_excess_total > 0.0f && mine > 0.0f) {
                    delta += pl->p.thermal_rate * 0.5f * up_largest * (mine / up_excess_total);
                }
            }

            pl->scratch[c] = delta;
        }
    }
}

static void stage_thermal_apply(const Planes* pl, int j0, int j1) {
    for (int j = j0; j < j1; j++) {
        for (int i = 0; i < pl->res; i++) {
            size_t c = idx(pl, i, j);
            float d = pl->scratch[c];
            pl->h[c] += d;
            if (d < 0.0f)
                pl->wear_acc[c] += -d;
            else
                pl->dep_acc[c] += d;
        }
    }
}

// Scale a mask to its own peak. Single-threaded on purpose: a threaded reduction
// would need a fixed combine order to stay deterministic, and this is one pass
// over the field against the hundreds the sim just ran.
static float normalise(float* plane, size_t n) {
    float peak = 0.0f;
    for (size_t k = 0; k < n; k++) {
        if (plane[k] > peak)
            peak = plane[k];
    }
    if (peak <= 0.0f)
        return 0.0f;
    float inv = 1.0f / peak;
    for (size_t k = 0; k < n; k++)
        plane[k] *= inv;
    return peak;
}

// FNV-1a over raw bytes. Single-threaded and in index order, because a digest
// computed in a varying order would answer a different question from the one
// asked -- and would be the one part of this file whose result depended on the
// thread count.
static void digest_plane(unsigned long long* h, const float* plane, size_t n) {
    const unsigned char* bytes = (const unsigned char*)plane;
    size_t total = n * sizeof(float);
    for (size_t k = 0; k < total; k++) {
        *h ^= (unsigned long long)bytes[k];
        *h *= 1099511628211ull;
    }
}

static double sum_plane(const float* plane, size_t n) {
    double total = 0.0;
    for (size_t k = 0; k < n; k++)
        total += (double)plane[k];
    return total;
}

bool terrain_erode(TerrainField* field, const TerrainParams* terrain, const ErosionParams* params,
                   ErosionStats* stats) {
    if (!field || !field->height || !terrain || !params)
        return false;
    if (field->res < 4 || params->iterations <= 0 || !(terrain->extent > 0.0f))
        return false;

    int res = field->res;
    size_t n = (size_t)res * (size_t)res;

    // One allocation for every scratch plane. Ten separate callocs would be ten
    // failure paths and ten frees; this is one of each.
    const int scratch_planes = 11;
    float* pool = calloc(n * (size_t)scratch_planes, sizeof(float));
    if (!pool)
        return false;

    Planes pl;
    memset(&pl, 0, sizeof(pl));
    pl.res = res;
    pl.cell = (2.0f * terrain->extent) / (float)(res - 1);
    pl.inv_cell = 1.0f / pl.cell;
    pl.p = *params;
    pl.h = field->height;
    pl.flow_acc = field->flow;
    pl.dep_acc = field->deposit;
    pl.wear_acc = field->wear;
    pl.w = pool + n * 0;
    pl.w_prev = pool + n * 1;
    pl.s = pool + n * 2;
    pl.s_next = pool + n * 3;
    pl.fl = pool + n * 4;
    pl.fr = pool + n * 5;
    pl.ft = pool + n * 6;
    pl.fb = pool + n * 7;
    pl.vx = pool + n * 8;
    pl.vz = pool + n * 9;
    pl.scratch = pool + n * 10;

    memset(field->flow, 0, n * sizeof(float));
    memset(field->deposit, 0, n * sizeof(float));
    memset(field->wear, 0, n * sizeof(float));

    double height_before = sum_plane(field->height, n);

    int workers = params->workers > 0 ? params->workers : get_cpu_cores();
    if (workers > EROSION_MAX_WORKERS)
        workers = EROSION_MAX_WORKERS;
    if (workers > res)
        workers = res;
    if (workers < 1)
        workers = 1;

    for (int it = 0; it < params->iterations; it++) {
        run_stage(&pl, stage_rain, workers);
        run_stage(&pl, stage_flux, workers);
        run_stage(&pl, stage_water, workers);
        run_stage(&pl, stage_capacity, workers);
        run_stage(&pl, stage_erode, workers);
        run_stage(&pl, stage_transport, workers);
        // The advected load lands in its own plane, so the swap is the barrier.
        float* tmp = pl.s;
        pl.s = pl.s_next;
        pl.s_next = tmp;
        run_stage(&pl, stage_evaporate, workers);

        if (params->thermal_every > 0 && (it % params->thermal_every) == 0) {
            run_stage(&pl, stage_thermal, workers);
            run_stage(&pl, stage_thermal_apply, workers);
        }
    }

    if (stats) {
        memset(stats, 0, sizeof(*stats));
        stats->height_before = height_before;
        stats->height_after = sum_plane(field->height, n);
        stats->sediment_left = sum_plane(pl.s, n);
        stats->eroded_total = sum_plane(field->wear, n);
        stats->deposited_total = sum_plane(field->deposit, n);
        stats->workers = workers;
    }

    // Flow is compressed before it is scaled, and the other two are not.
    //
    // Drainage is heavy-tailed by nature: a trunk channel carries orders of
    // magnitude more than the hillside beside it, so scaling linearly to the peak
    // leaves everything except the single largest river at nearly zero. log1p is
    // what every terrain tool displays flow accumulation through, for this reason.
    // Erosion and deposition are not heavy-tailed -- they are bounded by how much
    // material a cell had -- so compressing them would only flatten them.
    for (size_t k = 0; k < n; k++)
        field->flow[k] = log1pf(field->flow[k]);
    float flow_peak = normalise(field->flow, n);
    float deposit_peak = normalise(field->deposit, n);
    float wear_peak = normalise(field->wear, n);
    if (stats) {
        stats->flow_peak = flow_peak;
        stats->deposit_peak = deposit_peak;
        stats->wear_peak = wear_peak;
        // Taken AFTER normalisation, so it covers what a consumer will actually
        // read rather than an intermediate no caller ever sees.
        unsigned long long h = 14695981039346656037ull;
        digest_plane(&h, field->height, n);
        digest_plane(&h, field->flow, n);
        digest_plane(&h, field->deposit, n);
        digest_plane(&h, field->wear, n);
        stats->digest = h;
    }

    free(pool);
    return true;
}
