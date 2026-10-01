// Drops on glass (spec 13.12): beads that form on a pane the rain strikes and dry where it
// stopped, and drops that run down it trailing beads behind them. Pure arithmetic over cells in
// the pane's own frame -- it declares no sampler, the lit shader having none to give -- and
// what it hands back is a surface: a normal the drops tilt, a roughness they clear of the
// glass's smear, and the shift each drop's lens puts on the view through the pane. The sizes
// below are physical; rainGlassSize scales them all.
//
// Requires, included first: rain_surface.glsl (the cover, the wetness, the ripple activity and
// pcg4d), depth.glsl (projectionIsOrtho), `time`, `view`, `projection` and `passMode`.

uniform float uRainBeads;    // 1 = rain beads on this surface; see Material.rain_beads
uniform vec3 rainTravel;     // unit direction the rain travels, held after it stops
uniform float rainBeadClock; // seconds of Rain.bead_clock
uniform float rainGlassLens; // scale on how far a drop bends the view; 1 = physical
uniform float rainGlassSize; // scale on the drops' size and spacing; 1 = physical

// Two sizes of bead. Each cell, this many metres across, holds at most one bead at a time, up to
// this radius, living this long on the bead clock -- a life that divides RAIN_BEAD_CLOCK_WRAP.
const float RAIN_BEAD_CELL_L = 0.016;
const float RAIN_BEAD_R_L = 0.0045;
const float RAIN_BEAD_LIFE_L = 16.0;
const float RAIN_BEAD_CELL_S = 0.007;
const float RAIN_BEAD_R_S = 0.002;
const float RAIN_BEAD_LIFE_S = 4.0;
// The share of cells that hold a bead on a pane fully exposed and soaked.
const float RAIN_BEAD_FILL_L = 0.55;
const float RAIN_BEAD_FILL_S = 0.75;
// The running drops: lanes this wide across the pane, drops this far apart down each, and the
// trail of small beads one leaves behind it.
const float RAIN_RUN_LANE = 0.06;
const float RAIN_RUN_SPACING = 1.5;
const float RAIN_RUN_TRAIL = 0.25;
const float RAIN_RUN_FILL = 0.35;
// A cap's height over its radius. A bead on glass meets it at about 50 degrees, which a
// paraboloid this tall reaches at its rim.
const float RAIN_BEAD_HEIGHT = 0.7;
// Water's index less air's: how far a thin prism of water bends a ray, per unit of its slope.
const float RAIN_WATER_BEND = 0.333;
// The furthest a drop is allowed to turn the view, so a steep rim cannot fold it backwards.
const float RAIN_LENS_MAX = 0.6;
// The slope past which a ray inside the water meets the bead's surface beyond the critical
// angle, 48.6 degrees for water into air, and is reflected whole: tan(48.6 deg).
const float RAIN_TIR_SLOPE = 1.134;

vec4 rainBeadHash(vec2 cell, uint a, uint b) {
    return vec4(pcg4d(uvec4(uvec2(ivec2(cell)), a, b))) / 4294967296.0;
}

/*
 * One layer of beads at plane coordinates `st`, in metres: the slope of the water's surface
 * there (.xy, along the plane's axes) and how much of the point it covers (.z). A cell holds a
 * bead while its hash is under `fill`, so as the fill falls with the drying the beads go one at
 * a time rather than all thinning together. Each life of a cell is a fresh bead in a fresh
 * place, keyed on the life modulo the clock's wrap so the wrap moves nothing.
 */
vec3 rainBeadLayer(vec2 st, float cell, float rMax, float life, float fill, uint seed,
                   float footprint) {
    vec2 id = floor(st / cell);
    float u = rainBeadClock / life + rainBeadHash(id, seed, 0x632be5abu).z;
    float phase = fract(u);
    uint lifeKey = uint(mod(floor(u), RAIN_BEAD_CLOCK_WRAP / life));
    vec4 h = rainBeadHash(id, seed + lifeKey * 7919u, 0x85ebca77u);
    // Grows in, sits, and is absorbed into the film at the end of its life.
    float r = rMax * mix(0.35, 1.0, h.x) * smoothstep(0.0, 0.2, phase) *
              (1.0 - smoothstep(0.85, 1.0, phase));
    vec2 d = st - ((id + 0.5) * cell + (h.yz - 0.5) * (cell - 2.0 * rMax));
    float rho = length(d) / max(r, 1e-6);
    float edge = footprint / max(r, 1e-6);
    float cover = (h.w < fill ? 1.0 : 0.0) * (1.0 - smoothstep(1.0 - edge, 1.0, rho));
    // A paraboloid cap RAIN_BEAD_HEIGHT * r tall: slope -2 H d / r^2.
    return vec3(-2.0 * RAIN_BEAD_HEIGHT * d / max(r, 1e-6) * cover, cover);
}

/*
 * The drops running down, at plane coordinates `s` across and `t` up: lanes RAIN_RUN_LANE wide,
 * each with drops RAIN_RUN_SPACING apart sliding at the lane's own speed in fits and starts --
 * the drop's time runs fast and slow and never backwards -- a head, and a narrowing chain of
 * beads behind it. The path wanders across the lane as a function of height, so the trail lies
 * where the head went. `slide` is how steep the pane is: flat glass runs nothing.
 */
vec3 rainRunLayer(float s, float t, float slide, float fill, float footprint) {
    float lane = floor(s / RAIN_RUN_LANE);
    vec4 hl = rainBeadHash(vec2(lane, 0.0), 0x9e3779b9u, 0x7f4a7c15u);
    float speed = mix(0.05, 0.25, hl.x) * smoothstep(0.35, 0.7, slide);
    float rate = 6.2831853 * mix(0.3, 0.9, hl.y);
    float tau = time + 0.6 * sin(rate * time + 6.2831853 * hl.z) / rate;
    float u = (t + speed * tau) / RAIN_RUN_SPACING + hl.w;
    float above = fract(u) * RAIN_RUN_SPACING; // metres above this drop's head
    vec4 hc = rainBeadHash(vec2(lane, floor(u)), 0x2545f491u, 0x1b873593u);
    float live = hc.x < fill ? 1.0 : 0.0;
    float x = s - (lane + 0.5) * RAIN_RUN_LANE - 0.012 * sin(t * 9.0 + 6.2831853 * hl.z) -
              0.006 * sin(t * 23.0 + 6.2831853 * hl.w);

    float rx = mix(0.0025, 0.004, hc.y);
    float ry = 1.4 * rx;
    vec2 e = vec2(x / rx, (above - ry) / ry);
    float headCover = 1.0 - smoothstep(1.0 - footprint / rx, 1.0, length(e));
    vec2 headSlope = -2.0 * RAIN_BEAD_HEIGHT * vec2(e.x, e.y * rx / ry) * headCover;

    float along = (above - 2.0 * ry) / RAIN_RUN_TRAIL;
    float w = rx * 0.45 * (1.0 - along) * (0.6 + 0.4 * sin(above * 180.0));
    float inTrail = step(0.0, along) * step(along, 1.0);
    float trailCover = inTrail * (1.0 - smoothstep(w - footprint, w, abs(x)));
    vec2 trailSlope = vec2(-0.6 * RAIN_BEAD_HEIGHT * x / max(w, 1e-5), 0.0) * trailCover;

    return live * (headCover >= trailCover ? vec3(headSlope, headCover)
                                           : vec3(trailSlope, trailCover));
}

// Where a view direction, at infinity, lands on the screen.
vec2 rainViewUv(vec3 dir) {
    vec4 c = projection * vec4(mat3(view) * dir, 0.0);
    return c.xy / c.w * 0.5 + 0.5;
}

struct RainGlass {
    vec2 shift;       // added to the refraction sample's uv
    float shiftPerPx; // how fast that shift changes across a pixel, in uv
    float blocked;    // the share of the light from behind the drops reflect away, 0..1
};

/*
 * Bead the surface with rain and return what the transmission sample needs. `exposure` is the
 * cover rainWetSurface read at `P + Ng * RAIN_NORMAL_OFFSET`.
 *
 * The drops sit on the side the rain STRIKES, which is the face looking against its travel, so
 * that side's cover is asked for whichever face is seen. Glass the rain falls on shelters its
 * own underside -- a glass roof seen from below, asked about the point under it, would never
 * bead. (A wall pane does not shelter its lee in the cover map: the rain meets it nearly edge
 * on, and the map's slope bias stores it deeper than a point a hand behind it.) How squarely
 * the rain meets the pane sets how many there are, with the cover's spread for what wind and
 * turbulence carry onto a pane the mean rain only grazes.
 *
 * A drop's surface lights like any water, from either side of the pane, and is a lens: it
 * bends the view through it toward its own thicker middle, which is what turns a lamp behind
 * it into a small inverted image.
 *
 * Call it from control flow uniform over the draw: it takes screen derivatives.
 */
RainGlass rainGlassDrops(inout vec3 N, inout float roughness, vec3 Ng, vec3 P, vec3 V,
                         vec2 fragCoord, float exposure) {
    RainGlass g = RainGlass(vec2(0.0), 0.0, 0.0);
    if (uRainBeads <= 0.0 || rainWetness <= 0.0 || passMode >= 2)
        return g;
    float footprint = length(fwidth(P));

    vec3 Nf = dot(rainTravel, Ng) <= 0.0 ? Ng : -Ng;
    vec3 fall = vec3(0.0, -1.0, 0.0) - Nf * -Nf.y;
    float slide = length(fall);
    vec3 down = slide > 0.05 ? fall / slide
                             : normalize(cross(Nf, abs(Nf.x) < 0.9 ? vec3(1.0, 0.0, 0.0)
                                                                    : vec3(0.0, 0.0, 1.0)));
    vec3 across = cross(Nf, down);
    // In units of the drop size from here, so the whole field scales together: a slope is a
    // ratio of lengths, and does not change with it.
    float size = max(rainGlassSize, 1e-3);
    vec2 st = vec2(dot(P, across), -dot(P, down)) / size;
    footprint /= size;

    float struck = exposure;
    if (dot(Nf, Ng) < 0.0)
        struck = rainExposureSoft(P - Ng * RAIN_NORMAL_OFFSET, 6.2831853 * ign(fragCoord) + 1.7);
    float reach = struck * clamp(-dot(rainTravel, Nf) + RAIN_SPREAD_TAN, 0.0, 1.0);
    float fill = rainWetness * reach;

    vec3 big = rainBeadLayer(st, RAIN_BEAD_CELL_L, RAIN_BEAD_R_L, RAIN_BEAD_LIFE_L,
                             RAIN_BEAD_FILL_L * fill, 1u, footprint);
    vec3 small = rainBeadLayer(st + 0.37, RAIN_BEAD_CELL_S, RAIN_BEAD_R_S, RAIN_BEAD_LIFE_S,
                               RAIN_BEAD_FILL_S * fill, 2u, footprint);
    vec3 run = rainRunLayer(st.x, st.y, slide, RAIN_RUN_FILL * rainRippleActivity * reach,
                            footprint);
    vec3 beads = big.z >= small.z ? big : small;
    vec3 drop = run.z >= beads.z ? run : beads;

    // A pixel wider than a bead sees the beads' average: their tilt washes out into roughness.
    float resolve = 1.0 - smoothstep(0.5, 1.5, footprint / RAIN_BEAD_R_S);
    vec3 slope = drop.x * across - drop.y * down;
    // From either side: seen through the glass, the water's far surface still reflects back
    // into the room -- totally, past the critical angle at a bead's rim, which is what draws
    // each bead's outline against a dark street.
    // Each step is skipped where it has nothing to do, rather than done with a zero weight: a
    // renormalised normal and a square root of a square are not the values they started as, and
    // a pane the rain cannot reach has to come out as the glass it was.
    if (drop.z * resolve > 0.0) {
        vec3 n = normalize(Nf - slope);
        N = normalize(mix(N, dot(N, Nf) >= 0.0 ? n : -n, drop.z * resolve));
        roughness = mix(roughness, 0.05, drop.z * resolve);
    }
    if (resolve < 1.0 && fill > 0.0)
        roughness = sqrt(roughness * roughness + (1.0 - resolve) * 0.05 * fill);

    // The lens, from the image at infinity: the view bent toward the drop's thicker middle,
    // which is up its slope. Not faded with the resolve, unlike the tilt: where the drops are
    // smaller than a pixel the bend changes within it, and the spread that leaves in the
    // shift is what raises the transmission's mip -- a field of beads too fine to see is
    // still a diffuser, which is how a wet window reads from across a room.
    vec3 dir = -V;
    vec3 bend = RAIN_WATER_BEND * rainGlassLens * slope;
    float amount = length(bend);
    if (amount > RAIN_LENS_MAX)
        bend *= RAIN_LENS_MAX / amount;
    if (amount > 0.0 && !projectionIsOrtho())
        g.shift = rainViewUv(normalize(dir + bend)) - rainViewUv(dir);
    g.shiftPerPx = length(fwidth(g.shift));
    // The rim: steep enough that what comes through the pane is reflected back by the bead's
    // surface rather than passed, which draws each bead's dark outline. Not faded with the
    // resolve -- unresolved, it is the darkening a beaded pane has on average.
    g.blocked = drop.z * smoothstep(0.9 * RAIN_TIR_SLOPE, 1.1 * RAIN_TIR_SLOPE, length(drop.xy) /
                                                                               max(drop.z, 1e-4));
    return g;
}
