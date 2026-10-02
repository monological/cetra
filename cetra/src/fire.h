#ifndef _FIRE_H_
#define _FIRE_H_

#include <cglm/cglm.h>
#include <stdbool.h>

// FIRE_MAX and the per-fire caps, which the shaders' arrays are sized by.
#include "../shaders/include/fire_constants.glsl"

/*
 * Fire (spec 13.14): flames that burn, glow by their own temperature, and light the scene
 * with what they draw.
 *
 * TWO KINDS behind one renderer. A GRID fire is a combustion simulation on a box of cells
 * (Nguyen, Fedkiw and Jensen 2002, on GPU Gems 3's rasterised passes): sources breathe fuel
 * into it, fuel that is hot enough burns into heat and soot, the heat rises by buoyancy, and the
 * flicker is the flow's own turbulence rather than anything scripted. A FLAME is a candle's:
 * a spine of points that sways and stretches (Lamorlette and Foster 2002), with a teardrop of
 * glowing soot round it evaluated where it is drawn, so a room full of them costs nothing a
 * grid would.
 *
 * Both are drawn the same way: soot absorbs, and glows as a BLACKBODY at its temperature --
 * its absorption times Planck's radiance, in absolute units (Pegoraro and Parker 2006) -- so a
 * flame's brightness is a physical quantity beside the engine's nits rather than a ramp. And
 * the light a fire casts is DERIVED from what it draws: its luminous intensity is the volume
 * integral of that emission, its colour is the emission's, and a point light sits at its
 * centroid. A fire that dies down dims its room by itself.
 *
 * WORLD UNITS ARE METRES and temperatures are KELVIN. What this file owns is the fire, not
 * how it is drawn or stepped on the GPU (fire_render.h): no GL here.
 */

typedef enum FireKind {
    FIRE_GRID = 0,     // a simulated box of cells
    FIRE_FLAME = 1,    // a candle's structural flame
    FIRE_FLIPBOOK = 2, // a sheet of frames played on camera-facing cards
} FireKind;

/*
 * One card of a FLIPBOOK fire: a quad standing on `base` (its bottom centre, world metres),
 * `size` wide and tall, turned about the vertical to face the camera, playing the fire's sheet
 * `phase` of a loop out of step with the others so no two cards flicker together.
 */
typedef struct FireCard {
    vec3 base;
    vec2 size;
    float phase; // 0..1
} FireCard;

/*
 * A flipbook's sidecar, read from `<flipbook>.json` the first frame the fire runs: the sheet's
 * layout and playback, the luminance its brightest texel stands for, the physical size a frame
 * was made at, and each frame's luminous intensity at that size, centroid height (a fraction
 * of the frame) and the sheet's mean colour. ENGINE-OWNED.
 */
typedef struct FireFlipbook {
    bool loaded;
    bool failed; // the sidecar would not read; the fire draws and casts nothing
    int frames, cols, rows;
    int width, height; // pixels a frame
    float fps;
    float peak_nits;
    vec2 box;             // metres a frame spans, wide and tall
    float* intensity;     // cd a frame at `box`
    float* centroid_y;    // 0..1 up the frame
    float mean_intensity; // cd over the loop at `box`
    float mean_cast;      // cd over the loop from this fire's cards, as they are sized
    vec3 color;
} FireFlipbook;

typedef enum FireShape {
    FIRE_SHAPE_BOX = 0,     // `a` the centre, `b` the half-extents
    FIRE_SHAPE_SPHERE = 1,  // `a` the centre, `radius`
    FIRE_SHAPE_CAPSULE = 2, // `a` to `b`, `radius`: a log
} FireShape;

/*
 * Where a GRID fire burns, in world metres: where gas crosses the reaction front -- Nguyen et
 * al.'s implicit surface -- and leaves it at the fire's peak temperature with its reaction
 * coordinate at 1. `coverage` is the share of the shape alight at any moment, under a noise
 * that drifts in space and time, which is what breaks a log's length into separate tongues;
 * `lift` is the speed the gas leaves at (Nguyen's injection speed v_f, which with the reaction
 * sets how big the flame is).
 */
typedef struct FireSource {
    FireShape shape;
    vec3 a;
    vec3 b;
    float radius;
    float coverage; // 0..1
    float lift;     // m/s
} FireSource;

// A solid box no flow passes, in world metres: a firebox's back and sides, a hearth.
typedef struct FireBox {
    vec3 min;
    vec3 max;
} FireBox;

/*
 * What a fire burns like, after Nguyen, Fedkiw and Jensen 2002 (section numbers theirs).
 * Every field defaults to a wood fire's (a candle's where a FLAME differs). A field marked
 * TASTE has no measurement behind its default: it shapes the flame, and the paper hands it to
 * the animator too.
 */
typedef struct FireParams {
    float ambient;     // K, the air the fire burns in
    float temperature; // K the gas leaves the reaction front at, its peak (sec. 4.3)
    // 1/s the reaction coordinate falls at once gas has crossed the front: Nguyen's k, which
    // with the paper is 1, so the coordinate is one less the time since the gas ignited
    float reaction_rate;
    // K/s the hot gas sheds at its peak, falling as the fourth power of its rise above ambient:
    // Nguyen's c_T (eq. 17). TASTE -- with the lift, it sets how tall the visible flame is.
    float cooling;
    // 1/s the hot gas mixes with the room's air, cooling it and thinning its soot in proportion,
    // whatever the cell size: the entrainment a plume draws in. A coarse grid's own numerical
    // blur did this unasked and a fine one does not, so without it a bake's plume stays hot.
    float entrainment;
    float core;            // seconds after crossing the front a gas glows blue: the core's depth
    float expansion;       // 1/s the gas expands at in the core (sec. 3.2), filling the flame
    float soot_yield;      // ppm/s soot forms at while the gas reacts
    float soot_burnout;    // 1/s soot oxidises at in the flame's hot zone
    float soot_burnout_at; // K above which it does
    float smoke_fade;      // 1/s smoke thins at once it has cooled
    float buoyancy;        // scale on Boussinesq lift, g (T - T_amb) / T_amb; 1 = physical
    float vorticity;       // confinement (Fedkiw et al. 2001): the curls the grid loses, put back
    float wind_response;   // 0..1, how much of the scene's wind blows through the box

    float soot_absorption; // 1/m per ppm: soot's absorption coefficient in the visible
    float smoke_albedo;    // the share of what smoke takes from the light that it scatters
    float blue_core;       // nits per metre the core glows at: CH* and C2* emission (sec. 3)
    // 0..1, how far the eye has adapted to the fire: 1 = Nguyen's von Kries transform to the
    // white of a blackbody at `temperature` (sec. 5, eq. 23), which is what makes a fire read
    // yellow-white rather than the deep red its spectrum is; 0 = the spectrum as it is. TASTE
    // between the two, since a fire is rarely the only thing an eye is adapted to.
    float adaptation;
    float brightness; // scale on the emission drawn and the light cast; 1 = physical

    // FLAME only: the soot's peak amount, and how far the flame flickers (0 = still).
    float flame_soot;
    float flicker;
} FireParams;

typedef struct Fire {
    // ENGINE-OWNED: what the fire did, and what it cast, as the engine last read it. Read
    // freely, never write.
    int steps;      // fixed steps taken since the fire started
    int start_step; // the clock's step when it started, so `steps` counts from 0
    int pending;    // steps due this frame and not yet taken
    bool started;
    bool answered;      // the light below has been read back at least once
    float intensity;    // cd, the luminous intensity of the emission, optically thin
    vec3 centroid;      // world metres, the emission-weighted centre
    vec3 color;         // linear Rec.709, luminance 1: the emission's colour, as adapted
    float heat_release; // W, the heat the hot gas sheds, which in balance is what burning adds
    int grid[3];        // GRID: the cells simulated, from `size` and `cell`
    // FLAME: the spine, base to tip -- xyz and the radius there -- and each point's velocity.
    vec4 spine[FIRE_SPINE_POINTS];
    vec3 spine_velocity[FIRE_SPINE_POINTS];
    FireFlipbook book; // FLIPBOOK: the sidecar, once read
    float embers_base; // the embers' authored emissive strength, taken the first frame

    // SETTINGS: plain stores. Write them directly, at any time.
    char name[32];
    FireKind kind;
    bool enabled; // false = nothing burns, nothing draws, and the light goes dark
    // GRID: the box simulated, world metres, centre and full size, cut into cubic cells of
    // `cell` metres -- at most FIRE_GRID_MAX_* of them. `floor` makes its bottom face solid.
    // FLAME: `center` is the wick's tip, size.y the flame's height and size.x its width.
    vec3 center;
    vec3 size;
    float cell;
    bool floor;
    FireSource sources[FIRE_MAX_SOURCES];
    int source_count;
    FireBox obstacles[FIRE_MAX_OBSTACLES];
    int obstacle_count;
    // A chimney's draw: inside `draft` the air is driven up at `draft_speed` m/s, and keeping
    // the flow incompressible pulls the room's air in through the firebox's mouth to replace
    // it -- which is what takes a fireplace's smoke up the flue instead of into the room. A
    // speed of 0 is no chimney.
    FireBox draft;
    float draft_speed;
    FireParams params;
    // FLIPBOOK: the sheet's path without its suffixes -- `<flipbook>_color.png` and
    // `<flipbook>.json` -- and the cards it plays on.
    char flipbook[256];
    FireCard cards[FIRE_MAX_CARDS];
    int card_count;
    // A light the fire drives, borrowed: its intensity and colour each frame, and a point or
    // spot's position too. Its type, shadows and range stay the caller's. NULL drives none.
    struct Light* light;
    vec3 light_offset; // added to the centroid where a point or spot is placed
    // A material the fire makes glow, borrowed: logs whose emissive strength rises and falls
    // with the fire's intensity about the strength they were authored with. NULL drives none.
    struct Material* embers;
} Fire;

typedef struct FireSystem {
    // BY FUNCTION: fire_system_add.
    Fire fires[FIRE_MAX];
    int count;

    // SETTINGS: plain stores.
    // --fire-slice: draw slice `debug_slice` of GRID fire `debug_fire`'s field `debug_field`
    // (0 temperature, 1 soot, 2 fuel, 3 speed, 4 reaction) into a corner of the frame; -1 = none.
    int debug_field;
    int debug_fire;
    int debug_slice;
    float sim_hz;          // fixed steps a second
    int max_steps;         // most steps a frame catches up; past it the clock skips ahead
    int jacobi_iterations; // pressure solve iterations a step
    bool maccormack;       // second-order advection (Selle et al. 2008); false = semi-Lagrangian
    // Seconds a fire has already burnt when it starts, simulated in one go at its first frame,
    // so a scene opens on a fire rather than on a match. 0 = it lights at load.
    float warmup;
} FireSystem;

// Created empty with the defaults above.
FireSystem* create_fire_system(void);
void free_fire_system(FireSystem* fs);

// A wood fire's parameters.
void fire_params_defaults(FireParams* params, FireKind kind);

// The fire's chromatic adaptation as a matrix on linear Rec.709: Nguyen's von Kries transform,
// in Hunt-Pointer-Estevez cone space, from the white of a blackbody at the fire's peak
// temperature to D65, mixed with the identity by `adaptation` and scaled to keep that white's
// luminance. Applied to what the fire emits and to the light it casts.
void fire_adaptation(const FireParams* params, mat3 out);

// A new fire of `kind` with the defaults, named, appended; NULL past FIRE_MAX.
Fire* fire_system_add(FireSystem* fs, FireKind kind, const char* name);

// True while any fire is enabled: what decides whether anything simulates or draws. NULL is
// false.
bool fire_system_active(const FireSystem* fs);

// A GRID fire's cells along each axis, from `size` and `cell` and capped at FIRE_GRID_MAX_*,
// and its box, world metres: those cells about `center`.
void fire_grid_cells(const Fire* fire, int cells[3]);
void fire_grid_bounds(const Fire* fire, vec3 min, vec3 max);

struct Wind;
// The air a scene's wind moves at time `t`, m/s, horizontal: what blows through a fire. Zero
// for no wind, or one that states no air speed.
void fire_wind_air(const struct Wind* wind, double t, vec3 out);
// Advance the fires' clocks to time `t`: how many fixed steps each owes this frame
// (Fire.pending), and every FLAME's spine through them. The GRID steps themselves are taken on
// the GPU (fire_render.h), which reads `pending`. NULL is a no-op.
void fire_update(FireSystem* fs, const struct Wind* wind, double t);

// What a FLAME casts, from its spine: intensity, centroid and colour. Exact against what is
// drawn, since the same profile is integrated. Called by fire_update.
void fire_flame_light(Fire* fire);

// The fire's light onto `light`: a point or spot gets the intensity in candela at the centroid;
// an area panel gets the luminance that gives the same intensity along its normal. Nothing
// before the first answer.
struct Light;
void fire_drive_light(const Fire* fire, struct Light* light);

// The fire's embers' emissive strength, following its intensity about the strength the
// material was authored with. Nothing before the first answer, or with no embers.
void fire_drive_embers(Fire* fire);

// Where `card` is in its flipbook's loop at time `t`, in frames, 0..frames: what it casts and
// what it draws are both read there.
double fire_card_frame(const FireFlipbook* book, const FireCard* card, double t);

// The blackbody table the GPU is handed, FIRE_BB_LUT_SIZE RGBA texels: the Rec.709
// chromaticity (rgb over luminance) and log10 of the luminance, over FIRE_BB_T_MIN..MAX K.
const float* fire_blackbody_table(void);
// A blackbody at `kelvin` read from that table as the shader reads it: nits per channel into
// `rgb`, UNCLAMPED (blue goes negative below about 1900 K), and the luminance returned. Black
// below FIRE_BB_T_MIN.
float fire_blackbody(float kelvin, vec3 rgb);
// The blue core's colour, its band emission through the observer: Rec.709, luminance 1,
// unclamped like the blackbody.
void fire_blue_color(vec3 out);

// --fire-probe: the blackbody at a ladder of temperatures, and every fire's state.
void fire_probe_print(const FireSystem* fs);

#endif // _FIRE_H_
