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
    // x = 0 no film (fall back to the closed form), 1 an open coast, 2 a closed loop -- which
    // decides whether the column index wraps. y = seconds per history slot, z = index of the
    // newest slot, w = the beach slope the tips were measured against.
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

// An island's columns are a ring and a coast running off the bed's edge is not, which decides
// whether the partner of the last column is the first one or itself.
bool shoreFilmClosed() {
    return shoreFilmParams.x > 1.5;
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
    float best = 1.0e30;
    int bi = 0;
    for (int i = 0; i < SHORE_FILM_COLS; i++) {
        vec2 d = shoreFilmCols[i].xy - p;
        float d2 = dot(d, d);
        if (d2 < best) {
            best = d2;
            bi = i;
        }
    }

    /*
     * The partner is the INDEX-ADJACENT column on the side the point is on -- not the
     * second-nearest by distance, which is discontinuous and looked it.
     *
     * Second-nearest changes IDENTITY as you move along the shore: somewhere between every
     * pair of columns there is a locus where the runner-up swaps for a different column with a
     * different tip, and the blend jumps across it. That locus is a straight line, the jump is
     * a step in the swash edge, and the lens is fitted to that edge -- so it printed as a
     * straight crease in the water surface with the sea at a different height either side of
     * it. Only visible with the swash IN, because with the tide out the lens contributes
     * nothing for the step to be a step in.
     *
     * Choosing by side makes it continuous. Where the nearest column changes from i to i+1 the
     * point is equidistant, so both readings use the pair (i, i+1) at the same weight and meet.
     */
    vec2 o = shoreFilmCols[bi].xy;
    vec2 n = shoreFilmCols[bi].zw;
    vec2 tangent = vec2(-n.y, n.x);
    float along = dot(p - o, tangent);
    int other = along >= 0.0 ? bi + 1 : bi - 1;
    if (shoreFilmClosed())
        other = (other + SHORE_FILM_COLS) % SHORE_FILM_COLS;
    else
        other = clamp(other, 0, SHORE_FILM_COLS - 1);
    float spacing = max(length(shoreFilmCols[other].xy - o), 1.0e-4);

    /*
     * SMOOTHSTEPPED, and that is not a polish -- a linear blend creases at every column.
     *
     * Walking along the shore, a linear interpolation between columns has a derivative that
     * jumps at each one: the slope of the segment behind is (tip_i - tip_{i-1}) and the slope
     * ahead is (tip_{i+1} - tip_i), and those disagree. The lens is fitted to this edge and the
     * water's NORMAL is its derivative, so a C1 break shades as a hard line -- a terrace at
     * every column, in the water and in the wet sand alike, because both read this function.
     *
     * Smoothstep has zero derivative at its ends, so both sides meet at zero and the field is
     * C1 across the join. The value it interpolates is unchanged.
     */
    float t = clamp(abs(along) / spacing, 0.0, 1.0);
    s.a = bi;
    s.b = other;
    s.t = t * t * (3.0 - 2.0 * t);
    s.dist = sqrt(best);
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
