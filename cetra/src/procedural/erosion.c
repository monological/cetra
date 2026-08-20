#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "erosion.h"

#include "../thread.h"

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

    float* scratch;       // per-stage intermediate: capacity, then thermal deltas
    float* thermal_shed;  // what a cell gives away this thermal pass
    float* thermal_total; // and the excess it splits that across

    float* flow_acc; // mask accumulators
    float* dep_acc;
    float* wear_acc;

    ErosionParams p;
} Planes;

// Stages are written as one of these and run over disjoint row bands. Every
// stage reads only planes no stage-mate writes, which is what makes the bands
// independent and the result identical at any worker count.
typedef void (*StageFn)(const Planes*, int j0, int j1);

typedef struct StageJob {
    const Planes* pl;
    StageFn fn;
} StageJob;

static void stage_band(void* ctx, int j0, int j1) {
    const StageJob* job = (const StageJob*)ctx;
    job->fn(job->pl, j0, j1);
}

// One stage across the workers, joining before it returns.
//
// The join between stages IS the double buffer's enforcement: within a stage no
// cell reads a value another cell writes, and across stages every read is of a
// plane the previous stage finished. Remove the join and the sim still produces
// eroded-looking terrain, which is why the gate asserts thread counts against
// each other rather than trusting the output to look wrong.
//
// The band split itself lives in cetra_bake_bands, shared with the cloud noise
// bake. It was copied from there originally, and a copy of the one mechanism two
// callers depend on for bitwise-identical output is a fix that can land in one
// and miss the other.
static void run_stage(const Planes* pl, StageFn fn, int workers) {
    StageJob job = {pl, fn};
    cetra_bake_bands(pl->res, workers, stage_band, &job);
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
                // The same clamp the giving side applies, so the two halves put
                // the same fraction on the same pipe.
                //
                // Not bitwise-equal totals, and an earlier comment here claimed
                // they were: the giving side is one product and the receiving side
                // is a sum of four separately-rounded ones, so the budget closes to
                // rounding (5e-09 relative) rather than to zero. What the shared
                // clamp buys is that neither side can decide a DIFFERENT fraction
                // left, which is a leak of real material rather than of last bits.
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

// Thermal erosion, gather form, in two passes.
//
// Written as a gather because the scatter form of this algorithm writes its
// neighbours' heights, and two threads settling adjacent cliffs then race -- the
// whole reason this sim can claim a thread count does not reach its output.
//
// TWO passes rather than one, and that is the shape the algorithm actually has.
// A single pass has to work out what each cell RECEIVES, which means knowing how
// much each higher neighbour is shedding and how it splits that between its own
// neighbours -- so it recomputed every neighbour's full eight-way excess scan
// inline. Every cell in the grid therefore scanned itself nine times: once for
// its own shed, and once more from each of eight neighbours. `h` does not change
// between them, so all nine agreed by construction and eight were waste. Pass A
// writes the two numbers pass B needs, and neighbour evaluations per cell go from
// 72 to 16. Bit-identical: the same operands in the same association, and the
// digest arm confirms it.
static const int THERMAL_NX[8] = {-1, 1, 0, 0, -1, 1, -1, 1};
static const int THERMAL_NZ[8] = {0, 0, -1, 1, -1, -1, 1, 1};

// Diagonal neighbours are a longer run for the same drop, so they get the
// diagonal threshold -- without it a cliff comes out with eight-fold symmetry
// stamped into it. Symmetric under direction reversal, which is what lets pass B
// derive a neighbour's excess toward this cell without rescanning it.
static inline float thermal_run(const Planes* pl, int k) {
    float drop = pl->p.talus * pl->cell;
    return (k < 4) ? drop : drop * 1.41421356f;
}

// Pass A: what each cell sheds, and the total it splits that across.
static void stage_thermal_shed(const Planes* pl, int j0, int j1) {
    for (int j = j0; j < j1; j++) {
        for (int i = 0; i < pl->res; i++) {
            size_t c = idx(pl, i, j);
            float here = pl->h[c];
            float total = 0.0f, largest = 0.0f;
            for (int k = 0; k < 8; k++) {
                if (!inside(pl, i + THERMAL_NX[k], j + THERMAL_NZ[k]))
                    continue;
                float diff = here - pl->h[idx(pl, i + THERMAL_NX[k], j + THERMAL_NZ[k])];
                float run = thermal_run(pl, k);
                if (diff > run) {
                    float e = diff - run;
                    total += e;
                    largest = e > largest ? e : largest;
                }
            }
            // A fraction of the single largest excess, not of the sum: shedding
            // the sum would let one cell move more than its own relief in a step
            // and invert the slope it was smoothing.
            pl->thermal_shed[c] = total > 0.0f ? pl->p.thermal_rate * 0.5f * largest : 0.0f;
            pl->thermal_total[c] = total;
        }
    }
}

// Pass B: what each cell receives, into scratch, from the two planes above.
static void stage_thermal(const Planes* pl, int j0, int j1) {
    for (int j = j0; j < j1; j++) {
        for (int i = 0; i < pl->res; i++) {
            size_t c = idx(pl, i, j);
            float here = pl->h[c];
            float delta = -pl->thermal_shed[c];

            for (int k = 0; k < 8; k++) {
                int ni = i + THERMAL_NX[k], nj = j + THERMAL_NZ[k];
                if (!inside(pl, ni, nj))
                    continue;
                size_t n = idx(pl, ni, nj);
                float total = pl->thermal_total[n];
                if (!(total > 0.0f))
                    continue;
                // This cell's share of what the neighbour sheds. thermal_run is
                // symmetric under reversal, so the threshold the neighbour used
                // for us is the one we compute for it.
                float mine = (pl->h[n] - here) - thermal_run(pl, k);
                if (mine > 0.0f)
                    delta += pl->thermal_shed[n] * (mine / total);
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

    // One allocation for every scratch plane, so there is one failure path and
    // one free rather than thirteen of each.
    const int scratch_planes = 13;
    float* pool = calloc(n * (size_t)scratch_planes, sizeof(float));
    if (!pool)
        return false;

    // Zero-initialised even though every field is assigned below, so it zeroes
    // nothing that stays zero. It is here for the field that gets ADDED later and
    // not assigned: a forgotten pointer is then NULL rather than a stack address
    // the stages write through.
    Planes pl = {0};
    pl.res = res;
    pl.cell = terrain_field_cell(terrain->extent, res);
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
    pl.thermal_shed = pool + n * 11;
    pl.thermal_total = pool + n * 12;

    memset(field->flow, 0, n * sizeof(float));
    memset(field->deposit, 0, n * sizeof(float));
    memset(field->wear, 0, n * sizeof(float));

    double height_before = sum_plane(field->height, n);

    int workers = cetra_bake_workers(params->workers, res);

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
            run_stage(&pl, stage_thermal_shed, workers);
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
        stats->closure = (stats->height_after + stats->sediment_left) - stats->height_before;
        double scale = stats->height_before != 0.0 ? fabs(stats->height_before) : 1.0;
        stats->closure_rel = fabs(stats->closure) / scale;
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
    // The sim moved material, so the field's stated range is stale. Recorded now
    // rather than left for a caller: terrain_tint normalises altitude against it,
    // and every path that installs a field has to leave it true.
    terrain_field_measure(field);

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

void erosion_stats_probe(const ErosionStats* stats, const ErosionParams* params, int res,
                         float cell, double ms) {
    if (!stats || !params)
        return;
    printf("terrain-erosion-probe header res=%d iterations=%d workers=%d cell=%.4f ms=%.0f\n", res,
           params->iterations, stats->workers, (double)cell, ms);
    // Nothing in a rendered frame can check this: terrain with a silently leaking
    // sediment budget still looks like eroded terrain.
    printf("terrain-erosion-probe budget before=%.6f after=%.6f suspended=%.6f closure=%.6f "
           "rel=%.3e\n",
           stats->height_before, stats->height_after, stats->sediment_left, stats->closure,
           stats->closure_rel);
    printf("terrain-erosion-probe moved eroded=%.6f deposited=%.6f\n", stats->eroded_total,
           stats->deposited_total);
    // Pre-normalisation peaks. A peak of zero means the sim ran and did nothing,
    // which normalising to the peak would otherwise hide by scaling noise to 1.
    printf("terrain-erosion-probe peaks flow=%.6f deposit=%.6f wear=%.6f\n",
           (double)stats->flow_peak, (double)stats->deposit_peak, (double)stats->wear_peak);
    // The determinism claim in one number. The budget rows above cannot make it:
    // addition hides compensating differences, so two thread counts that disagree
    // cell by cell can still report identical totals.
    printf("terrain-erosion-probe digest value=%016llx\n", stats->digest);
}
