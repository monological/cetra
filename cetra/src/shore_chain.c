#include <math.h>
#include <string.h>

#include "shore_chain.h"

#include "water.h"

// Substeps per frame. The chain is centimetre-scale segments carrying metre-per-second water,
// so one step at 60 Hz is well past CFL; four is what keeps a bore front stable.
#define SHORE_CHAIN_SUBSTEPS 4
/*
 * EVERY DYNAMIC CONSTANT HERE IS PER METRE and is converted at use, because the chain works in
 * world units and a world unit is not a metre.
 *
 * `a = -g d(eta)/dx` has eta and x in world units, so g has to be too -- and the CFL caps
 * below bound a world-unit velocity. Written as metres and used raw, apps/tree's 22 units to
 * the metre turned a 6 m/s cap into 0.27 world-units/s: the chain could not move faster than a
 * centimetre a frame, every column sat at exactly its seeded rest position, and the probe read
 * a tip spread of zero. That is the same mis-scaling spec 11.44 found across the water's other
 * physical lengths, arriving here through the one quantity that is an acceleration.
 */
#define SHORE_CHAIN_GRAVITY_M 9.81f
// Bed friction at the seaward end, and how much more of it at the tip. A swash thins as it
// climbs and a thin sheet is nearly all boundary layer, so the drag it feels is not constant.
#define SHORE_CHAIN_FRICTION 0.3f
#define SHORE_CHAIN_FRICTION_TIP 3.0f
// Artificial viscosity on COMPRESSION only, which is what stops a bore front from going
// vertical and then negative. Expansion is left alone: a draining sheet is not a shock.
#define SHORE_CHAIN_VISC 0.25f
#define SHORE_CHAIN_VISC_CAP 0.5f
// CFL caps. Without them a segment that briefly collapses hands back an acceleration large
// enough to throw its node clean off the beach in one step.
#define SHORE_CHAIN_ACCEL_CAP_M 25.0f
#define SHORE_CHAIN_SPEED_CAP_M 6.0f
// Alongshore smoothing per frame. Columns are independent solvers, so without this they
// decorrelate into a comb at the column spacing -- the same artefact ocean.glsl records from
// reading the beach slope per vertex.
#define SHORE_CHAIN_SMOOTH 0.04f
// The film's rest depth at its seaward end, METRES: the isobath the run-up hands over at.
#define SHORE_CHAIN_REST_DEPTH_M 0.25f
// Floor on a segment's length as a fraction of its rest length, so `vol / length` cannot
// divide by nothing when a bore piles up.
#define SHORE_CHAIN_MIN_SEG 0.2f
// How much of the analytic run-up reaches the handover node as its own excursion. See the
// drive: the rest of the distance is the chain's to produce.
#define SHORE_CHAIN_DRIVE 0.18f

static float _clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void shore_chain_rebuild(ShoreChain* chain, const Water* water,
                         const ShoreRunupParams* params) {
    if (!chain || !water || !water->shore_pts || water->shore_count < 3) {
        if (chain)
            chain->ready = false;
        return;
    }

    /*
     * The chain reaches from the handover isobath up to the run-up ceiling, so its rest span is
     * a physical length rather than a tuned one: below it the wave field still owns the surface,
     * above it no wave can reach whatever it does.
     */
    const float upm = params->units_per_metre;
    const float slope = shore_runup_slope(params);
    const float seaward = SHORE_CHAIN_REST_DEPTH_M / slope * upm;
    const float landward = shore_runup_ceiling(params);
    const float span = seaward + (landward > 0.0f ? landward : upm);
    const bool changed = !chain->ready || fabsf(span - chain->rest_span) > 1.0e-3f ||
                         chain->wraps != water->shore_closed;

    // Resample the polyline to a fixed column count, so the solver's cost does not follow the
    // tracer's resolution.
    //
    // The cursor is OUTSIDE the column loop. Both the requested arc length and the polyline's
    // own are monotone in their index, so it never needs to go backwards -- restarting it per
    // column made this a rescan of the whole polyline sixty-four times a frame, which is the
    // opposite of the property the comment claimed.
    int k = 0;
    for (int j = 0; j < SHORE_CHAIN_COLS; j++) {
        const float f = (float)j / (float)SHORE_CHAIN_COLS;
        const float s = f * water->shore_length;
        while (k + 1 < water->shore_count && water->shore_pts[k + 1].s < s)
            k++;
        const WaterShorePoint* a = &water->shore_pts[k];
        chain->origin[j * 2] = a->x;
        chain->origin[j * 2 + 1] = a->z;
        chain->normal[j * 2] = a->nx;
        chain->normal[j * 2 + 1] = a->nz;
    }
    chain->wraps = water->shore_closed;

    if (!changed)
        return;

    /*
     * Seed at rest: nodes evenly spaced from the isobath to the ceiling, still, with each
     * segment holding the water a wedge of that slope would. The rest state is what the film
     * relaxes back to, so it has to BE the still-water wedge or the chain drifts on frame one.
     */
    chain->rest_span = span;
    chain->seaward_x = -seaward;
    const float seg = span / (float)(SHORE_CHAIN_NODES - 1);
    for (int i = 0; i < SHORE_CHAIN_NODES - 1; i++) {
        const float mid = -seaward + ((float)i + 0.5f) * seg;
        const float depth = mid < 0.0f ? -mid * slope : 0.0f;
        chain->vol[i] = seg * depth;
    }
    for (int j = 0; j < SHORE_CHAIN_COLS; j++)
        for (int i = 0; i < SHORE_CHAIN_NODES; i++) {
            chain->x[j * SHORE_CHAIN_NODES + i] = -seaward + (float)i * seg;
            chain->u[j * SHORE_CHAIN_NODES + i] = 0.0f;
        }
    memset(chain->tips, 0, sizeof(chain->tips));
    chain->head = 0;
    chain->slot_clock = 0.0f;
    chain->steps = 0;
    chain->ready = true;
}

static void _step_column(ShoreChain* chain, int j, float slope, float upm, float drive,
                         float drive_u, float sub) {
    const float gravity = SHORE_CHAIN_GRAVITY_M * upm;
    const float accel_cap = SHORE_CHAIN_ACCEL_CAP_M * upm;
    const float speed_cap = SHORE_CHAIN_SPEED_CAP_M * upm;
    float* x = &chain->x[j * SHORE_CHAIN_NODES];
    float* u = &chain->u[j * SHORE_CHAIN_NODES];
    const float rest_seg = chain->rest_span / (float)(SHORE_CHAIN_NODES - 1);
    const float floor_len = SHORE_CHAIN_MIN_SEG * rest_seg;

    /*
     * Surface elevation per segment: the sand it stands on plus the water column over it. The
     * column is the segment's conserved volume divided by its CURRENT length, which is what
     * makes squeezing the chain raise the surface and push back -- the whole reason this is a
     * pressure form and not a spring.
     */
    float eta[SHORE_CHAIN_NODES - 1];
    for (int i = 0; i < SHORE_CHAIN_NODES - 1; i++) {
        const float len = fmaxf(x[i + 1] - x[i], floor_len);
        const float du = u[i + 1] - u[i];
        // Compression only. An expanding segment is a sheet draining, not a shock.
        const float q = du < 0.0f ? fminf(SHORE_CHAIN_VISC * du * du, SHORE_CHAIN_VISC_CAP)
                                  : 0.0f;
        eta[i] = slope * 0.5f * (x[i] + x[i + 1]) + chain->vol[i] / len + q;
    }

    for (int i = 1; i < SHORE_CHAIN_NODES; i++) {
        // eta sits at segment midpoints; at the tip there is no segment beyond, so its own
        // terrain stands in -- and the spacing is the half-segment to it rather than a whole
        // one, or the tip feels half the gravity it should and lags the chain behind it.
        const float eta_r = i < SHORE_CHAIN_NODES - 1 ? eta[i] : slope * x[SHORE_CHAIN_NODES - 1];
        const float dx =
            fmaxf((i < SHORE_CHAIN_NODES - 1 ? x[i + 1] - x[i - 1] : x[i] - x[i - 1]) * 0.5f,
                  rest_seg);
        float a = -gravity * (eta_r - eta[i - 1]) / dx;
        a = _clampf(a, -accel_cap, accel_cap);
        const float fr = SHORE_CHAIN_FRICTION *
                         (1.0f + SHORE_CHAIN_FRICTION_TIP * (float)i /
                                     (float)(SHORE_CHAIN_NODES - 1));
        u[i] += (a - fr * u[i]) * sub;
        u[i] = _clampf(u[i], -speed_cap, speed_cap);
    }

    // The seaward node is driven, not solved: it is the handover to the wave field.
    x[0] = drive;
    u[0] = drive_u;
    for (int i = 1; i < SHORE_CHAIN_NODES; i++)
        x[i] += u[i] * sub;

    // Nodes may not pass each other. A crossed pair is negative volume, and the eta above
    // would hand back a force pushing them further apart the wrong way. Same floor the depth
    // uses -- it was spelled a second time here under a second name, which read as two
    // independent limits that happened to agree.
    for (int i = 1; i < SHORE_CHAIN_NODES; i++)
        if (x[i] < x[i - 1] + floor_len) {
            x[i] = x[i - 1] + floor_len;
            if (u[i] < u[i - 1])
                u[i] = u[i - 1];
        }
}

// One Jacobi pass of alongshore smoothing over a field, wrapping or clamping at the ends.
static void _smooth(float* a, int offset, bool wrap) {
    float prev_col[SHORE_CHAIN_COLS];
    for (int j = 0; j < SHORE_CHAIN_COLS; j++)
        prev_col[j] = a[j * SHORE_CHAIN_NODES + offset];
    const int lo = wrap ? 0 : 1;
    const int hi = wrap ? SHORE_CHAIN_COLS : SHORE_CHAIN_COLS - 1;
    for (int j = lo; j < hi; j++) {
        const int p = (j + SHORE_CHAIN_COLS - 1) % SHORE_CHAIN_COLS;
        const int n = (j + 1) % SHORE_CHAIN_COLS;
        a[j * SHORE_CHAIN_NODES + offset] +=
            SHORE_CHAIN_SMOOTH * (prev_col[p] + prev_col[n] - 2.0f * prev_col[j]);
    }
}

void shore_chain_step(ShoreChain* chain, const ShoreRunupParams* params, float t, float dt) {
    if (!chain || !chain->ready || dt <= 0.0f)
        return;
    const float slope = shore_runup_slope(params);
    const float slot_interval = shore_runup_slot_interval(params);
    // Capped, so a frame spike does not step the solver past what its CFL caps can absorb.
    const float sub = fminf(dt, 0.04f) / (float)SHORE_CHAIN_SUBSTEPS;

    float drive[SHORE_CHAIN_COLS], drive_u[SHORE_CHAIN_COLS];
    for (int j = 0; j < SHORE_CHAIN_COLS; j++) {
        /*
         * The drive is the analytic run-up's edge at this column's own waterline point, mapped
         * onto the beach normal: a height above the still level becomes a distance along the
         * face by the slope. The chain then answers what the water DOES with that forcing.
         */
        const float ox = chain->origin[j * 2], oz = chain->origin[j * 2 + 1];
        const float e = shore_runup_edge(params, ox, oz, t);
        const float e_prev = shore_runup_edge(params, ox, oz, t - dt);
        // The junction's own excursion, not the run-up itself. The analytic edge is already
        // an answer to "how far does the water get"; feeding it in whole would drive the
        // handover node up the beach and leave the chain nothing to do. What the junction
        // actually does is oscillate by a fraction of it, and the chain's dynamics carry that
        // the rest of the way -- overshooting a drained face and stalling against a full one,
        // which is the behaviour a closed form cannot have. The fraction is tuned, not
        // derived: the shallow-water excursion it stands in for needs the wave's own elevation
        // at the isobath, and what is available here is the edge it produced.
        const float excursion = SHORE_CHAIN_DRIVE * e / slope;
        drive[j] = chain->seaward_x + excursion;
        drive_u[j] = _clampf(SHORE_CHAIN_DRIVE * ((e - e_prev) / slope) / dt,
                             -SHORE_CHAIN_SPEED_CAP_M * params->units_per_metre,
                             SHORE_CHAIN_SPEED_CAP_M * params->units_per_metre);
    }

    for (int s = 0; s < SHORE_CHAIN_SUBSTEPS; s++)
        for (int j = 0; j < SHORE_CHAIN_COLS; j++)
            _step_column(chain, j, slope, params->units_per_metre, drive[j], drive_u[j], sub);

    for (int i = 0; i < SHORE_CHAIN_NODES; i++) {
        _smooth(chain->x, i, chain->wraps);
        _smooth(chain->u, i, chain->wraps);
    }

    /*
     * THE HEAD SLOT IS WRITTEN EVERY FRAME; only its ADVANCE is on the slot interval. Those are
     * two different things and collapsing them breaks opposite ends of the same feature.
     *
     * The head is what a consumer reads at age 0 -- the water's edge NOW, which drives the sea's
     * lens and the waterline. It has to move every frame or the surf visibly steps: recording
     * the whole slot on the interval left the live edge holding a value for two seconds at a
     * time, so the waves stopped coming in and switched position instead.
     *
     * The OLDER slots are the drying history, and they want the opposite. The consumer marches
     * back over SHORE_TAP_PERIODS of wave time in SHORE_TAPS steps, so advancing per frame gave
     * SHORE_CHAIN_HISTORY frames of it -- a fifth of a second at 60 Hz -- and every tap but the
     * first ran off the end of the ring. Nine taps returned two distinct values and the wet sand
     * collapsed into the two-tone step the accumulation exists to avoid.
     *
     * Writing the head each frame and advancing on the interval gives both: an edge that is live
     * and a ring that spans the window the taps reach over.
     */
    for (int j = 0; j < SHORE_CHAIN_COLS; j++) {
        // Back to a HEIGHT above the still level, which is what the shaders compare against.
        chain->tips[chain->head][j] =
            chain->x[j * SHORE_CHAIN_NODES + SHORE_CHAIN_NODES - 1] * slope;
    }

    chain->slot_clock += dt;
    if (chain->slot_clock >= slot_interval) {
        // Carry the remainder rather than zeroing: dropping it would make the true interval the
        // frame time rounded up, which drifts against what the shader is told it is.
        chain->slot_clock -= slot_interval;
        // The new head starts as a copy of the one it follows, so it is a real tip from its
        // first frame rather than a zero the consumer would read as the water being at the
        // still line. The loop above overwrites it next frame.
        const int prev = chain->head;
        chain->head = (chain->head + 1) % SHORE_CHAIN_HISTORY;
        memcpy(chain->tips[chain->head], chain->tips[prev], sizeof(chain->tips[prev]));
    }
    chain->steps++;
}
