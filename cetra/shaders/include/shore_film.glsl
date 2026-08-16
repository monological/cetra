/*
 * THE SWASH FILM's tips, as a uniform block (spec 11.45).
 *
 * A closed-form run-up says where the water's edge ought to be. It cannot say that THIS wave
 * ran into the last one's backwash and stopped short, because that is a collision between two
 * bodies of water and a formula has none. The film simulates it -- a Lagrangian chain per
 * alongshore column -- and what comes back here is the only part a shader needs: how far up
 * the beach each column's water reached, kept for the last few frames.
 *
 * A UNIFORM BLOCK, not a texture, and that is what makes this affordable at all. pbr_frag has
 * declared sixteen samplers since 4.10 and the driver counts declarations, so a texture here
 * would have needed a whole unit freed first. A few hundred floats is uniform space, which
 * clustered forward already established costs nothing from that ledger.
 *
 * Read by BOTH the water and the lit surfaces it runs over, so the sea's lens and the sand's
 * wetness are driven by the same tips and agree by construction rather than by two
 * calibrations that drift.
 */

#define SHORE_FILM_COLS 64
#define SHORE_FILM_SLOTS 12

layout(std140) uniform ShoreFilmBlock {
    // x = active (0 = no film, fall back to the closed form), y = seconds per history slot,
    // z = index of the newest slot, w = the beach slope the tips were measured against.
    vec4 shoreFilmParams;
    // Each column's waterline point and its landward normal: xy = world position, zw = normal.
    vec4 shoreFilmCols[SHORE_FILM_COLS];
    // Tip HEIGHT above the still level, world units, packed four to a vec4 and indexed
    // slot * SHORE_FILM_COLS + column.
    vec4 shoreFilmTips[(SHORE_FILM_COLS * SHORE_FILM_SLOTS) / 4];
};

bool shoreFilmActive() {
    return shoreFilmParams.x > 0.5;
}

float shoreFilmTip(int slot, int col) {
    int i = slot * SHORE_FILM_COLS + col;
    return shoreFilmTips[i >> 2][i & 3];
}

/*
 * The two columns nearest a world point, and how far between them it sits.
 *
 * A LINEAR SEARCH over 64 columns, which is the honest cost of not having a texture. It runs
 * only where the swash can reach -- every caller gates on the run-up's own analytic ceiling
 * first, so dry land and open water pay one compare rather than this.
 *
 * The alternative was an alongshore coordinate baked into the bed's spare channel, and it is
 * worth recording why that is not here: such a coordinate is an arc length, an arc length on a
 * closed shore has a CUT where the chain's ends meet, and interpolating across it sweeps the
 * whole coast. For a SMOOTH periodic quantity like a tip that would actually be harmless --
 * the two ends are neighbours and blending them is right -- but pbr_frag cannot sample the bed
 * at all, and one lookup that works in both programs beats two that disagree.
 */
struct ShoreFilmSample {
    int a, b;  // the two nearest columns
    float t;   // 0 at a, 1 at b
    float dist; // distance to the shoreline, world units
    bool found;
};

ShoreFilmSample shoreFilmNearest(vec2 p) {
    ShoreFilmSample s;
    s.a = 0;
    s.b = 0;
    s.t = 0.0;
    s.dist = 0.0;
    s.found = false;
    if (!shoreFilmActive())
        return s;
    float best = 1.0e30, second = 1.0e30;
    int bi = 0, si = 0;
    for (int i = 0; i < SHORE_FILM_COLS; i++) {
        vec2 d = shoreFilmCols[i].xy - p;
        float d2 = dot(d, d);
        if (d2 < best) {
            second = best;
            si = bi;
            best = d2;
            bi = i;
        } else if (d2 < second) {
            second = d2;
            si = i;
        }
    }
    // Weighted by distance rather than by projecting onto the segment between them: the
    // columns are a resampling of a traced polyline and need not be evenly spaced, and a
    // projection would need the segment's own length to mean anything.
    float da = sqrt(best), db = sqrt(second);
    s.a = bi;
    s.b = si;
    s.t = da + db > 1.0e-6 ? da / (da + db) : 0.0;
    s.dist = da;
    s.found = true;
    return s;
}

// The film's swash edge at a point, `age` seconds ago, as a HEIGHT above the still level --
// the same quantity shoreRunup's edge is, so a caller can use one in place of the other.
float shoreFilmEdge(ShoreFilmSample s, float age) {
    float perSlot = max(shoreFilmParams.y, 1.0e-4);
    float f = clamp(age / perSlot, 0.0, float(SHORE_FILM_SLOTS - 1));
    int s0 = int(f);
    int s1 = min(s0 + 1, SHORE_FILM_SLOTS - 1);
    float ft = f - float(s0);
    int head = int(shoreFilmParams.z);
    // The history is a ring, so a slot `n` back is head - n modulo the depth.
    int i0 = (head - s0 + SHORE_FILM_SLOTS * 2) % SHORE_FILM_SLOTS;
    int i1 = (head - s1 + SHORE_FILM_SLOTS * 2) % SHORE_FILM_SLOTS;
    float e0 = mix(shoreFilmTip(i0, s.a), shoreFilmTip(i0, s.b), s.t);
    float e1 = mix(shoreFilmTip(i1, s.a), shoreFilmTip(i1, s.b), s.t);
    return mix(e0, e1, ft);
}
