#ifndef _FIRE_H_
#define _FIRE_H_

#include <cglm/cglm.h>
#include <stdbool.h>

#include "light.h"

// FIRE_MAX and the per-fire caps, which the shaders' arrays are sized by.
#include "../shaders/include/fire_constants.glsl"

/*
 * Fire (spec 13.14): flames that burn, glow by their own temperature, and light the scene
 * with what they draw.
 *
 * THREE KINDS behind one renderer. A GRID fire is a combustion simulation on a box of cells
 * (Nguyen, Fedkiw and Jensen 2002, on GPU Gems 3's rasterised passes): sources breathe fuel
 * into it, fuel that is hot enough burns into heat and soot, the heat rises by buoyancy, and the
 * flicker is the flow's own turbulence rather than anything scripted. A FLAME is a candle's:
 * a spine of points that sways and stretches (Lamorlette and Foster 2002), with a teardrop of
 * glowing soot round it evaluated where it is drawn, so a room full of them costs nothing a
 * grid would. A FLIPBOOK plays a sheet of frames baked offline on cards turned to the camera.
 *
 * The first two are drawn the same way: soot absorbs, and glows as a BLACKBODY at its
 * temperature -- its absorption times Planck's radiance, in absolute units (Pegoraro and Parker
 * 2006) -- so a flame's brightness is a physical quantity beside the engine's nits rather than a
 * ramp. And the light every kind casts is DERIVED from what it draws: its luminous intensity is
 * the integral of that emission, its colour is the emission's, and a point light sits at its
 * centroid. A fire that dies down dims its room by itself.
 *
 * A fire's geometry is LOCAL to its origin: the world position of the node it hangs on, or the
 * world's origin with none. Only the node's position is taken, not its rotation or scale: a
 * flame rises against gravity whatever the carrier does -- a tilted torch still burns upright --
 * and a simulated box stays aligned with the world its buoyancy is stated in. A fire on no node
 * stays at the world's origin through an origin shift, since it has no place of its own to move:
 * a fire in a world that shifts hangs on a node.
 *
 * WORLD UNITS ARE METRES and temperatures are KELVIN. What this file owns is the fire, not
 * how it is drawn or stepped on the GPU (fire_render.h): no GL here. The one texture a fire
 * holds, a flipbook's sheet, comes from the scene's texture pool.
 */

typedef enum FireKind {
    FIRE_GRID = 0,     // a simulated box of cells
    FIRE_FLAME = 1,    // a candle's structural flame
    FIRE_FLIPBOOK = 2, // a sheet of frames played on camera-facing cards
    FIRE_KIND_COUNT
} FireKind;

typedef enum FireShape {
    FIRE_SHAPE_BOX = 0,     // `a` the centre, `b` the half-extents
    FIRE_SHAPE_SPHERE = 1,  // `a` the centre, `radius`
    FIRE_SHAPE_CAPSULE = 2, // `a` to `b`, `radius`: a log
} FireShape;

/*
 * Where a GRID fire burns: where gas crosses the reaction front -- Nguyen et al.'s implicit
 * surface -- and leaves it at the fire's peak temperature with its reaction coordinate at 1.
 * `coverage` is the share of the shape alight at any moment, under a noise that drifts in space
 * and time, which is what breaks a log's length into separate tongues; `lift` is the speed the
 * gas leaves at (Nguyen's injection speed v_f, which with the reaction sets how big the flame
 * is).
 */
typedef struct FireSource {
    FireShape shape;
    vec3 a;
    vec3 b;
    float radius;
    float coverage; // 0..1
    float lift;     // m/s
} FireSource;

// A box: a solid no flow passes (a firebox's back and sides, a hearth), or a chimney's flue.
typedef struct FireBox {
    vec3 min;
    vec3 max;
} FireBox;

// A GRID fire: a box of `size` metres about `center`, cut into cubic cells of `cell` metres --
// at most FIRE_GRID_MAX_* of them. `floor` makes its bottom face solid.
typedef struct FireGrid {
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
} FireGrid;

// A FLAME: a wick's tip, and the flame's width and height above it.
typedef struct FireFlame {
    vec3 wick;
    float width;
    float height;
} FireFlame;

/*
 * One card of a FLIPBOOK fire: a quad standing on `base` (its bottom centre), `size` wide and
 * tall -- 0 for the size the sheet's frames were made at -- turned about the vertical to face
 * the camera, playing the sheet `phase` of a loop out of step with the others so no two cards
 * flicker together.
 */
typedef struct FireCard {
    vec3 base;
    vec2 size;
    float phase; // 0..1
} FireCard;

typedef struct FireCards {
    FireCard list[FIRE_MAX_CARDS];
    int count;
} FireCards;

/*
 * A flipbook, as fire_set_flipbook read it from its sidecar: the sheet the sidecar names beside
 * itself, its layout and playback, the luminance its brightest texel stands for, the size a
 * frame was made at, and each frame's luminous intensity at that size and centroid height (a
 * fraction of the frame). The sheet is stored bottom row first: frame k sits at column
 * k % cols of row k / cols, row 0 at the bottom.
 */
// The longest path a flipbook's sidecar or sheet may have, as a scene file's paths.
#define FIRE_PATH_MAX 1024
// The most a fire's vigour reads: twice as hard as it usually burns is a fire flaring, and a
// mean taken over only a few answers should not make a spark read as a blaze.
#define FIRE_VIGOUR_MAX 2.0f

typedef struct FireFlipbook {
    char path[FIRE_PATH_MAX]; // the sidecar; "" = none
    // The sheet, from the scene's texture pool, held: NULL until a sidecar and its sheet have
    // both read, which is what every use of the rest asks.
    struct Texture* sheet;
    int frames, cols, rows;
    int width, height; // pixels a frame
    int gutter;        // transparent pixels round each frame in the sheet
    float fps;
    float peak_nits;
    vec2 box;             // metres a frame spans, wide and tall
    float* intensity;     // cd a frame at `box`
    float* centroid_y;    // 0..1 up the frame
    float mean_intensity; // cd over the loop at `box`
    vec3 color;           // linear Rec.709, luminance 1
} FireFlipbook;

/*
 * What a fire burns like, after Nguyen, Fedkiw and Jensen 2002 (section numbers theirs).
 * Every field defaults to a wood fire's (a candle's where a FLAME differs). A field marked
 * TASTE has no measurement behind its default: it shapes the flame, and the paper hands it to
 * the animator too. A FLIPBOOK reads only `brightness`: the rest of its look is in its sheet.
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
    // blur did this unasked and a fine one does not, so without it a fine plume stays hot.
    float entrainment;
    float core;            // seconds after crossing the front a gas glows blue: the core's depth
    float expansion;       // 1/s the gas expands at in the core (sec. 3.2), filling the flame
    float soot_yield;      // ppm/s soot forms at while the gas reacts
    float soot_burnout;    // 1/s soot oxidises at in the flame's hot zone
    float soot_burnout_at; // K above which it does
    float smoke_fade;      // 1/s smoke thins at once it has cooled
    float buoyancy;        // scale on Boussinesq lift, g (T - T_amb) / T_amb; 1 = physical
    float vorticity;       // confinement (Fedkiw et al. 2001): the curls the grid loses, put back
    float wind_response;   // 0..1, how much of the scene's wind blows through the fire

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
    bool answered;   // the light below has been read at least once
    float intensity; // cd, the luminous intensity of the emission, optically thin
    vec3 centroid;   // world metres, the emission-weighted centre
    vec3 color;      // linear Rec.709, luminance 1: the emission's colour, as adapted
    // How hard the fire is burning against how it usually does: its intensity over a running
    // mean (a FLIPBOOK's over its loop's), 1 = as usual, at most FIRE_VIGOUR_MAX. What its
    // embers glow with, and what a crackle can swell with.
    float vigour;
    float heat_release; // GRID only: W, the heat the hot gas sheds, which in balance is what
                        // burning adds
    vec3 origin;        // world metres, `node`'s position as fire_update last read it
    // fire_adaptation's matrix for `params`, as fire_update last saw them: what the emission is
    // drawn and cast through.
    mat3 adaptation;
    // FLAME: the spine, base to tip, in world metres -- xyz and the radius there -- and each
    // point's velocity. World rather than local, so a flame carried along trails behind.
    vec4 spine[FIRE_SPINE_POINTS];
    vec3 spine_velocity[FIRE_SPINE_POINTS];
    FireFlipbook flipbook; // FLIPBOOK: fire_set_flipbook

    // BY FUNCTION.
    FireKind kind;           // fire_system_add
    struct Material* embers; // fire_set_embers

    // SETTINGS: plain stores. Write them directly, at any time.
    char name[32];
    bool enabled; // false = nothing burns, nothing draws, and the light goes dark
    // The node the fire hangs on, borrowed: its world position is the fire's origin. NULL = the
    // world's origin.
    struct SceneNode* node;
    FireParams params;
    // A light the fire drives, borrowed: its intensity and colour each frame, a point or
    // spot's position too, and a FLAME's point light's body -- its direction, source radius and
    // source length, from the spine. Its type, shadows and range stay the caller's. NULL drives
    // none.
    struct Light* light;
    vec3 light_offset; // added to the centroid where a point or spot is placed
    float light_scale; // the light's intensity over what the fire casts; 1 = physical
    // What the kind burns from, local to the origin: the one member `kind` names.
    union {
        FireGrid grid;
        FireFlame flame;
        FireCards cards;
    };

    // ENGINE-OWNED, behind the settings they follow: the cache keys of `adaptation`, and the
    // mean `vigour` is taken against -- a plain average of the first answers until there have
    // been enough for the running one, so a fire that lights small is not measured against its
    // first flicker.
    float adapted_for[2];
    float mean_intensity;
    int mean_samples;
    // The light as the fire last drove it, kept while fire_system_hold holds it at rest.
    Light held;
    bool holding;
} Fire;

typedef struct FireSystem {
    // ENGINE-OWNED: the air the scene's wind moves through every fire this frame, m/s.
    vec3 air;

    // BY FUNCTION: fire_system_add.
    Fire fires[FIRE_MAX];
    int count;

    // SETTINGS: plain stores.
    // --fire-slice: draw slice `debug_slice` of GRID fire `debug_fire`'s field `debug_field`
    // (FireField) into a corner of the frame; -1 = none.
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

// The fields --fire-slice can show of a GRID fire, by these names.
typedef enum FireField {
    FIRE_FIELD_TEMPERATURE = 0,
    FIRE_FIELD_SOOT = 1,
    FIRE_FIELD_REACTION = 2, // Nguyen's reaction coordinate Y
    FIRE_FIELD_SPEED = 3,
    FIRE_FIELD_CORE = 4, // the blue core's weight
    FIRE_FIELD_COUNT
} FireField;
extern const char* const FIRE_FIELD_NAMES[FIRE_FIELD_COUNT];

// Created, or initialised in storage the caller owns, empty with the defaults above.
FireSystem* create_fire_system(void);
void fire_system_init(FireSystem* fs);
// Frees what the fires loaded, and the system.
void free_fire_system(FireSystem* fs);

// A new fire of `kind` with its kind's defaults, named, appended; NULL past FIRE_MAX.
Fire* fire_system_add(FireSystem* fs, FireKind kind, const char* name);

// A GRID source with the defaults: a box 5 cm about the origin, half of it alight, the gas
// leaving at half a metre a second.
FireSource fire_source_default(void);

struct TexturePool;
// A FLIPBOOK's sheet, from the sidecar at `path` -- a JSON naming the sheet beside it -- loaded
// into `pool`, which a scene's fire takes from that scene. False, said once, when either will
// not read; the fire then draws and casts nothing. Load time: it reads two files.
bool fire_set_flipbook(Fire* fire, struct TexturePool* pool, const char* path);

// The material the fire's embers glow through: its emission is scaled by the fire's vigour, and
// the material it replaces is handed back unscaled. NULL glows nothing.
void fire_set_embers(Fire* fire, struct Material* embers);

// True while any fire is enabled: what decides whether anything simulates or draws. NULL is
// false.
bool fire_system_active(const FireSystem* fs);

struct Wind;
// Advance the fires to time `t`, after the transform walk: each fire's origin from its node, the
// steps it owes this frame (Fire.pending), every FLAME's spine through them, and what each FLAME
// and FLIPBOOK casts. A GRID fire's steps are taken on the GPU (fire_render.h), which reads
// `pending`. NULL is a no-op.
void fire_update(FireSystem* fs, const struct Wind* wind, double t);

struct SceneNode;
// Hand what every fire cast to what it drives, once this frame's light is known: its light,
// placed in the frame of the node it hangs on under `root`, its vigour over `dt` seconds, and
// its embers. A fire that is out, or has cast nothing yet, darkens its light.
void fire_system_drive(FireSystem* fs, struct SceneNode* root, float dt);

// While the light is captured (spec 13.42): `rest` drives each fire's light from what the fire
// casts at rest -- a FLAME standing straight and still, a FLIPBOOK at its loop's mean -- and its
// embers at the vigour it usually burns with, keeping the light as it was; false puts both back.
// A GRID fire's light has no rest and is left as it burns.
void fire_system_hold(FireSystem* fs, struct SceneNode* root, bool rest);

// The world moved by -`delta` (an origin shift): what fires hold in world space moves with it.
void fire_system_shift_origin(FireSystem* fs, const vec3 delta);

// --fire-probe: the blackbody at a ladder of temperatures, and every fire's state.
void fire_probe_print(const FireSystem* fs);

#endif // _FIRE_H_
