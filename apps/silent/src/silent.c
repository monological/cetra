/*
 * silent: a grimy kitchen at night, its fluorescent tubes buzzing, and a
 * street of houses outside the window that goes on into fog. Walk it in first
 * person, with a flashlight.
 *
 * Every mesh is flat-shaded boxes and prisms from kit.c, and every texture a
 * CC0 photo scan cut down to 256 px -- aimed at the look of a console-era
 * survival horror game: few polygons, low texel density, the dirt in the
 * textures rather than modelled, and darkness and fog hiding how little is
 * there.
 *
 * It names one internal header, the profiler, for --profiler's report.
 */

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cglm/cglm.h>

#include "cetra/camera.h"
#include "cetra/engine.h"
#include "cetra/gi_volume.h"
#include "cetra/ibl.h"
#include "cetra/light.h"
#include "cetra/postfx.h"
#include "cetra/probe.h"
#include "cetra/probe_set.h"
#include "cetra/rain.h"
#include "cetra/scene.h"
#include "cetra/shadow.h"
#include "cetra/sky.h"
#include "cetra/wind.h"
#include "cetra/internal/profiler.h"

#include "cetra/game/audio.h"
#include "cetra/game/entity.h"
#include "cetra/game/game.h"
#include "cetra/game/input.h"
#include "cetra/game/physics.h"

#include "candles.h"
#include "cat.h"
#include "cat_brain.h"
#include "cat_debug.h"
#include "cat_voice.h"
#include "clock.h"
#include "door.h"
#include "hearth.h"
#include "prompt.h"
#include "rain_bed.h"
#include "house.h"
#include "interior.h"
#include "kitchen.h"
#include "kit.h"
#include "layout.h"
#include "lights.h"
#include "mats.h"
#include "player.h"
#include "sounds.h"
#include "street.h"
#include "study.h"

#define DEFAULT_WIDTH  1600
#define DEFAULT_HEIGHT 900
// Half the window's pixels, upscaled: the engine's floor.
#define DEFAULT_RENDER_SCALE 0.5f

/*
 * The exposure at night, pinned, and the most a day's meter may open to. The
 * sky is photometric, like the tubes, so by day one meter serves the street
 * and the rooms: it closes for the overcast outside and opens indoors, never
 * past what the night is pinned at.
 */
#define EXPOSURE_NIGHT 0.014f
// How long the view takes to come up from black once the room's bounce light
// is in.
#define FADE_IN_SECONDS 1.0f
// What a day's meter maps the frame's mean to. Middle grey puts a fog world at
// middle grey, where a camera in fog is opened a stop and a third so the fog
// reads white.
#define DAY_METER_KEY 0.45f
// The local exposure by day (spec 13.19). The window onto the street sits about four stops over
// that key, and the key leaves about one above it before white, so the base above grey keeps a
// fifth of its height. The window also pulls the meter down, so the room sits under grey and the
// base below it is lifted. Blend 0: a blurred base averages the dark walls into the window's and
// compresses it less.
#define DAY_LE_HIGHLIGHTS 0.2f
#define DAY_LE_SHADOWS    0.6f
#define DAY_LE_BLEND      0.0f
// Moderate rain, by the meteorologists' bands (rain.h): steady enough to soak the
// street and fill its gutters, short of a downpour that would hide it.
#define DEFAULT_RAIN_MMH 6.0f
// Rain the cat finds as interesting as rain gets, mm/h: the default is about half of it.
#define CAT_HEAVY_RAIN 12.0f
// The rain is art-directed here, and has to be. In fog this dense a drop refracts glowing air
// about as bright as itself, so rain at its physical opacity shows only right under a lamp --
// true of real rain in fog, and not what this street is for. So each streak is brighter than
// the drops it stands for, and they are packed closer round the player, where a streak is big
// enough to read. It is also slower and sparser than the rate implies, and catches half the
// engine's default sheen: this is a drizzle to walk through, not weather to fight.
#define RAIN_STREAK_BRIGHTNESS 3.0f
#define RAIN_STREAK_RADIUS     2.5f
#define RAIN_STREAK_COUNT      6144
#define RAIN_FALL_SCALE        0.5f
#define RAIN_STREAK_SHEEN      1.0f
// Wet ground at half the physical darkening: soaked, but the lamps still pool on it.
#define RAIN_WET_DARKENING 0.5f
// Splash droplets twice their physical size: at their own a splash on dark wet asphalt lifts
// its pixels by a tenth, and the ground round the player's feet reads as still.
#define RAIN_SPLASH_SIZE 2.0f
// Drops on glass at twice and a half their size: physical beads are a few millimetres, under
// a pixel of the kitchen window from anywhere in the kitchen at the render scale this runs at.
#define RAIN_GLASS_DROP_SIZE 2.5f
// Drips at sixteen times their opacity: one drop smeared over the shutter is a faint line a
// few pixels wide even at arm's length, and the gutter's leaks past the kitchen window would
// not read as water.
#define RAIN_DRIP_BRIGHTNESS 16.0f
// The puddles stand in the road's and the yard's own lows, fully.
#define RAIN_PUDDLE_RELIEF 1.0f
// A breeze off the street onto the front of the house, gusting. In still air the eave keeps
// the rain off the facade and the kitchen window; this drives it onto their lower halves. The
// speed is a gust's peak; the lulls between fall to 1 - WIND_GUST_AMOUNT of it.
#define WIND_AIR_SPEED      2.5f
#define WIND_GUST_FREQUENCY 0.6f
#define WIND_GUST_AMOUNT    0.5f

typedef struct SilentArgs {
    bool headless;
    int frames;
    const char* screenshot;
    int screenshot_every;
    int width, height;
    int seed;
    bool day;
    bool no_taa;
    bool no_grade;
    int local_exposure;      // 1 on, 0 off; -1 = this mode's own choice
    float le_highlights;     // the local exposure's highlight contrast; below 0 = this mode's own
    float le_shadows;        // and below it
    float le_blend;          // and its blurred-luminance blend
    bool le_probe;           // print the frame's luminance percentiles it is asked to fit
    bool negative_probe;     // print how much of the frame reaches the tonemap below zero
    bool exposure_probe;     // print what the meter decided, every frame it decides
    bool no_history_rescale; // histories keep the exposure they were written at (A/B)
    float render_scale;
    int msaa;
    bool cam_eye_set, cam_target_set;
    vec3 cam_eye, cam_target;
    float fov_deg;
    const char* pad_script;
    bool trace_player;
    bool no_flicker;
    bool flashlight;
    bool mute;
    float rain_mmh;         // 0 = dry
    bool no_wind;           // still air: the rain falls straight
    bool no_relief;         // puddles from the noise alone, not the ground's own lows
    bool no_candles;        // the candles stand unlit
    bool no_candle_shadows; // the candles light through walls, as before spec 13.16
    bool no_gi;             // no bounce light: no GI volume, and no reflection probes from it
    int tile_views;         // views over each cached light's body; 0 = the engine's, 1 = one
    bool tiles_probe;       // print the cached shadow tiles at exit
    bool profiler;          // per-pass timing and submission counts, reported at exit
    const char* audio_dump; // headless: write what the listener hears here
    bool no_cat;
    vec3 cat_fur, cat_eyes; // sRGB
    const char* cat_at;     // a place by name, or NULL for home
    char cat_clip[32];      // a clip by name, or empty for the place's own
    float cat_clip_seconds; // held this far in; below 0 it plays
    bool no_eyeshine;
    char cat_go[32];       // a place it sets off for, or empty to stay
    CatGait cat_gait;      // and how fast
    bool cat_cam;          // the camera follows the cat
    bool trace_cat;        // print what it is doing every 30 steps
    bool cat_say;          // it makes every sound it has, in turn
    unsigned int cat_seed; // its mind's seed
    bool cat_blind;        // it does not see or hear the player
    const char* cat_doing; // the activity it starts with, or NULL to choose
} SilentArgs;

static SilentArgs g_args;
static Scene* g_scene;
static Player g_player;
static Lights g_lights;
static Clock g_clock;
static RainBed g_rain_bed;
static Sounds g_sounds;
static Cat g_cat;
static CatMind g_mind;
static CatVoice g_voice;

// The door that opens, and the line that says what the action key would do. It answers when
// the eye is within DOOR_REACH of its leaf's middle and looking within DOOR_CONE of it.
#define DOOR_REACH 1.9f
#define DOOR_CONE  0.6f // radians
static Door g_door;
static bool g_door_hung;
static Prompt g_prompt;

// --audio-dump: the offline mix, pulled a frame's worth at a time so it keeps
// step with the sim clock, as interleaved stereo at the engine's rate.
#define DUMP_RATE 48000
static float* g_dump;
static size_t g_dump_frames, g_dump_cap;
static float g_fade_seconds; // since the bounce light came in

// The spawn: in the kitchen, facing the window.
static const vec3 SPAWN_FEET = {1.5f, FLOOR_Y, 13.2f};
static const float SPAWN_YAW = GLM_PIf; // toward -z, the street

// Every action, one table: input_bind takes a single table and borrows it. The
// player reads the move, sprint, look and cursor rows by name (player.h); the
// flashlight and the GUI are read here.
static const InputAction ACTIONS[] = {
    {"move_x",
     {INPUT_KEY(D, 1), INPUT_KEY(A, -1), INPUT_AXIS(LEFT_X, 1), INPUT_PAD(DPAD_RIGHT, 1),
      INPUT_PAD(DPAD_LEFT, -1)}},
    {"move_y",
     {INPUT_KEY(W, 1), INPUT_KEY(S, -1), INPUT_AXIS(LEFT_Y, -1), INPUT_PAD(DPAD_UP, 1),
      INPUT_PAD(DPAD_DOWN, -1)}},
    {"sprint", {INPUT_KEY(LEFT_SHIFT, 1), INPUT_PAD(LEFT_BUMPER, 1)}},
    {"look_x", {INPUT_AXIS(RIGHT_X, 1), INPUT_KEY(RIGHT, 1), INPUT_KEY(LEFT, -1)}},
    {"look_y", {INPUT_AXIS(RIGHT_Y, -1), INPUT_KEY(UP, 1), INPUT_KEY(DOWN, -1)}},
    {"flashlight", {INPUT_KEY(F, 1), INPUT_PAD(Y, 1)}},
    {"interact", {INPUT_KEY(E, 1), INPUT_PAD(X, 1)}},
    {"release_cursor", {INPUT_KEY(TAB, 1)}},
    {"toggle_gui", {INPUT_KEY(GRAVE_ACCENT, 1), INPUT_KEY(G, 1)}},
};

/*
 * The sky: the Hillaire atmosphere in nits, and the IBL baked from it -- which
 * is what lights the street, and what the fog's ambient follows. At night the
 * sun is well under the horizon with the moon and the stars up; by day it is an
 * overcast noon. apps/tree's setup, with this world's metre.
 */
static void build_sky(Engine* engine) {
    SkyAtmosphere* sky = create_sky_atmosphere();
    IBLResources* ibl = create_ibl_resources();
    if (!sky || !ibl) {
        free_sky_atmosphere(sky);
        free_ibl_resources(ibl);
        return;
    }
    sky->world_units_per_km = 1000.0f;
    // In nits, like every light in the house.
    sky->radiance_scale = SKY_PHOTOMETRIC_SCALE;
    if (g_args.day) {
        // An overcast noon: cloud covers the whole sky, so no sun gets
        // through, nothing casts, and the dome lights everything evenly.
        sky->sun_elevation_deg = 35.0f;
        sky->sun_azimuth_deg = 150.0f;
        sky->overcast = 1.0f;
        sky->moon_enabled = false;
    } else {
        sky->sun_elevation_deg = -18.0f;
        sky->sun_azimuth_deg = 250.0f;
        sky->stars_enabled = true;
        sky->night_floor_enabled = true;
        sky->moon_enabled = true;
        sky->moon_elevation_deg = 32.0f;
        sky->moon_azimuth_deg = 200.0f;
        sky->moon_size = 3.0f;
    }
    sky_update_moon(sky);
    sky_update_sun_dir(sky);
    // What a puddle or a wet car mirrors is the fog over the street, not the
    // navy sky above it. Resolved from the post chain's fog at the bake, so
    // build_post has to have run first.
    ibl->reflect_fog = true;
    if (sky_bake_static_luts(sky, engine) != 0 || sky_bake(sky, ibl, engine) != 0) {
        free_sky_atmosphere(sky);
        free_ibl_resources(ibl);
        return;
    }
    g_scene->sky = sky;
    g_scene->ibl = ibl;
    g_scene->render_skybox = true;
    g_scene->skybox_brightness = 1.0f;
    g_scene->skybox_ground_projection = false;

    LightDesc sun_desc = {
        .name = "sun", .type = LIGHT_DIRECTIONAL, .size = {4.0f, 4.0f}, .cast_shadows = true};
    Light* sun = create_light(&sun_desc);
    // Dark in both modes as authored -- under the deck by day, under the
    // horizon at night -- and here so the GUI's sun and overcast still have a
    // light to drive, at the level where the sun agrees with the sky.
    sky->sun_light = sun;
    sky->sun_base_intensity = SKY_SUN_ILLUMINANCE;
    sky_apply_sun_to_light(sky);
    scene_add_light(g_scene, sun);
    SceneNode* sun_node = create_node();
    node_set_name(sun_node, "sun");
    node_set_light(sun_node, sun);
    node_add_child(g_scene->root_node, sun_node);
    // No moon LIGHT: the street at night is its lamps and the night floor, and
    // the moon is the disc in the sky.
}

/*
 * Irradiance probes over the house, so a room is lit by what it can see: the
 * tubes' light off its own walls, and not the sky, which the environment's
 * irradiance would otherwise pour into every closed room through the roof.
 *
 * The grid is placed so no probe centre lands in a wall or a floor. Probes sit
 * at cell centres, and the walls of two storeys and an octagonal tower leave no
 * one-metre spacing clear of all of them; 1.21 m cells from (-7.45, 7.58) put
 * every column at least 0.15 m off every wall's face, the tower's eight
 * included, and seven layers to 9.4 m keep every row off the floors, the
 * ceilings and the tower's. A probe inside a wall sees only backfaces and
 * darkens everything near it.
 *
 * The fireplace's hood and breast stand out of the back wall further than any
 * spacing could clear, so a row of probes is inside them. That is harmless as
 * a wall's is not: a wall is two layers, and a probe in one sees the other's
 * face, while the hearth's solids are closed shells, culled from inside, so a
 * probe there sees the room past them.
 *
 * Outside the grid a query clamps to the nearest edge probes, which stand in
 * the front yard -- the right kind of answer for the street, which is lit
 * mostly by its own lamps and the moon rather than by what bounces.
 */
#define GI_CELL  1.21f
#define GI_COLS  11
#define GI_ROWS  7
#define GI_TOP   9.4f
#define GI_CLEAR 0.15f // how near a probe centre may come to a wall's or a slab's face

static void build_gi(void) {
    GIVolume* gi = create_gi_volume(GI_COLS, GI_ROWS, GI_COLS);
    if (!gi)
        return;
    const vec3 lo = {-7.45f, 0.0f, 7.58f};
    gi_volume_fit(gi, lo, (vec3){lo[0] + GI_COLS * GI_CELL, GI_TOP, lo[2] + GI_COLS * GI_CELL});
    if (!scene_add_gi_volume(g_scene, gi))
        return;

    // Every centre against the walls and slabs, so a layout change that walks one into a wall
    // says so here rather than as a dark patch.
    int near = 0;
    for (int i = 0; i < gi->counts[0]; i++)
        for (int j = 0; j < gi->counts[1]; j++)
            for (int k = 0; k < gi->counts[2]; k++) {
                const vec3 c = {gi->grid_min[0] + ((float)i + 0.5f) * gi->spacing[0],
                                gi->grid_min[1] + ((float)j + 0.5f) * gi->spacing[1],
                                gi->grid_min[2] + ((float)k + 0.5f) * gi->spacing[2]};
                const float d = house_clearance(c);
                if (d >= GI_CLEAR)
                    continue;
                if (near++ < 8)
                    printf("silent: GI probe at (%.2f, %.2f, %.2f) is %.2f m from a wall or slab\n",
                           (double)c[0], (double)c[1], (double)c[2], (double)d);
            }
    printf("silent: GI %d probes, %d nearer than %.2f m to a wall or slab\n",
           gi->counts[0] * gi->counts[1] * gi->counts[2], near, (double)GI_CLEAR);
}

/*
 * Reflection probes in the kitchen, the hall, the great hall, the study and the
 * study's tower bay. Without them every metal and every wet surface indoors
 * reflects the only environment there is, the night sky, and the hood, the sink
 * and the floor go black. The great hall's box goes up to the ridge, since the
 * hall is open to its roof: a roof outside every box reflects the sky.
 *
 * The study is TWO boxes, the room to its ceiling and the octagonal bay to the
 * bay's high one, because one box round both reached past the house's west wall
 * beside the tower, past its front wall east of it and up through the roof over
 * the room, and the facade and the roof there reflected the study. A box still
 * cannot follow the bay's diagonal faces, so their outside takes the bay's probe.
 *
 * The engine captures them once the irradiance volume has converged, so they
 * see the rooms lit by it rather than by the open sky's ambient.
 *
 * A box runs out to the plane its doors and panes hang in: the CENTRE line of a
 * wall it shares with another room, where a leaf hangs; the room-side face of an
 * outside wall's pane, half a pane short of its centre line, which takes in the
 * reveals and the wedge between the wall tops and the roof; and, for the hall,
 * the front door's middle, since that door hangs against the inner face.
 * Stopping at the inner faces left all of those outside every box, reflecting the
 * day sky: the parlour door, seen at a grazing angle down the hall, went a
 * washed-out grey. Run to the front wall's centre, the hall's box would take in
 * the front door's street face too, and the street would see the hall in it.
 */
static void build_probes(void) {
    if (!g_scene->ibl || !g_scene->ibl->precomputed)
        return;
    enum { ROOMS = 5 };
    const struct {
        vec3 pos, lo, hi;
    } rooms[ROOMS] = {
        {{2.48f, FLOOR_Y + 1.5f, 11.9f},
         {HALL_X1, FLOOR_Y, HOUSE_FRONT_Z + KIT_PANE_HALF},
         {HOUSE_X1 - KIT_PANE_HALF, CEIL_Y, KITCHEN_BACK_Z}},
        {{-0.75f, FLOOR_Y + 1.5f, 12.0f},
         {HALL_X0, FLOOR_Y, FRONT_DOOR_Z},
         {HALL_X1, CEIL_Y, KITCHEN_BACK_Z}},
        {{HEARTH_X, FLOOR_Y + 1.8f, 16.6f},
         {HOUSE_X0 + KIT_PANE_HALF, FLOOR_Y, KITCHEN_BACK_Z},
         {HOUSE_X1 - KIT_PANE_HALF, house_roof_y(0.0f), HOUSE_BACK_Z - KIT_PANE_HALF}},
        {{-3.25f, FLOOR2_Y + 1.6f, 11.9f},
         {HOUSE_X0 + KIT_PANE_HALF, FLOOR2_Y, HOUSE_FRONT_Z + KIT_PANE_HALF},
         {HALL_X0, CEIL2_Y, KITCHEN_BACK_Z}},
        // Over the desk, which stands in the middle of the bay.
        {{TOWER_X, FLOOR2_Y + 1.8f, TOWER_Z},
         {TOWER_X - TOWER_APOTHEM + KIT_PANE_HALF, FLOOR2_Y,
          TOWER_Z - TOWER_APOTHEM + KIT_PANE_HALF},
         {TOWER_X + TOWER_APOTHEM, TOWER_CEIL_Y, TOWER_Z + TOWER_APOTHEM}},
    };
    ReflectionProbeSet* set = create_reflection_probe_set();
    if (!set)
        return;
    for (int i = 0; i < ROOMS; i++) {
        ReflectionProbe* p = create_reflection_probe();
        if (!p)
            break;
        glm_vec3_copy((float*)rooms[i].pos, p->position);
        glm_vec3_copy((float*)rooms[i].lo, p->box_min);
        glm_vec3_copy((float*)rooms[i].hi, p->box_max);
        vec3 span;
        glm_vec3_sub(p->box_max, p->box_min, span);
        p->near_clip = 0.02f;
        p->far_clip = 2.0f * glm_vec3_norm(span);
        // A probe's weight fades OUTWARD past its box, by this fraction of the
        // box's half-size on each axis. The default fifth carried the kitchen's
        // reflection 0.37 m out, through its 0.1 m wall and onto the great hall's
        // panelling. The boxes now meet at the planes their walls share, so there
        // is no doorway left for a fade to blend, and it is what stands between a
        // pane's two faces: half a pane along the longest axis, less along the
        // others (the fraction is of each axis's own half-size, and a distance once
        // stated here for every axis held only on the longest), and at least the
        // engine's floor of a thousandth of each, about 5 mm across the great hall.
        // That is still under the 6 mm of glass, so a window's street face is never
        // the room's.
        p->box_fade = KIT_PANE_HALF / (0.5f * glm_vec3_max(span));
        if (!probe_set_add(set, p)) {
            free_reflection_probe(p);
            break;
        }
    }
    if (set->count == ROOMS)
        g_scene->probe_set = set;
    else
        free_reflection_probe_set(set);
}

static void build_post(const Engine* engine, bool night, bool grade) {
    PostFX* fx = engine->postfx;
    if (!fx)
        return;
    postfx_apply_film_look(fx);
    fx->contact_shadows_enabled = true;
    // A puddle in the road mirrors a lamp head or a window across the street,
    // ten to twenty metres up its reflected ray. The default reach of eight fades
    // those out and leaves wet ground reflecting nothing.
    fx->ssr_max_distance = 40.0f;
    // The film look warms the highlights; this place is green-grey, and the
    // grade that makes it so is the LUT (tools/make_grade.py).
    glm_vec3_one(fx->grade_gain);
    if (grade && postfx_load_lut(fx, "assets/lut/silent_grade.cube"))
        fx->lut_strength = 1.0f;

    // A thin haze everywhere at night -- enough to put a beam in the flashlight
    // -- and the street's fog volumes on top of it. None by day: the ambient
    // that lights the haze is not blocked by walls, so at daylight's level it
    // fills the rooms like smoke, and the volumes carry the street on their own.
    fx->fog_enabled = true;
    fx->fog_density = night ? 0.02f : 0.0f;
    fx->fog_height_falloff = 60.0f;
    fx->fog_floor_y = 0.0f;
    fx->fog_far = 60.0f;
    fx->fog_anisotropy = 0.7f;
    // At night the fog's own glow, and not the sky's: the night sky's radiance
    // is a deep navy, and fog lit by it swallows the street into black. A dim
    // grey-green veil is what the houses fade INTO, which is what reads as fog.
    // By day the overcast dome lights it, which is the sky the fog fades into.
    if (night)
        postfx_set_fog_ambient(fx, (vec3){5.0f, 5.6f, 5.2f});

    // An exposure per pixel on top of the camera's (spec 13.19), which is what lets a window onto
    // the day come down without the kitchen around it going dark. By day only: the night's
    // exposure is pinned, and its look with it.
    fx->local_exposure_enabled = g_args.local_exposure < 0 ? !night : g_args.local_exposure == 1;
    if (!night) {
        fx->local_exposure_highlights = DAY_LE_HIGHLIGHTS;
        fx->local_exposure_shadows = DAY_LE_SHADOWS;
        fx->local_exposure_blend = DAY_LE_BLEND;
    }
    if (g_args.le_highlights >= 0.0f)
        fx->local_exposure_highlights = g_args.le_highlights;
    if (g_args.le_shadows >= 0.0f)
        fx->local_exposure_shadows = g_args.le_shadows;
    if (g_args.le_blend >= 0.0f)
        fx->local_exposure_blend = g_args.le_blend;
    fx->local_exposure_probe = g_args.le_probe;
    fx->negative_probe = g_args.negative_probe;
    fx->rescale_histories = !g_args.no_history_rescale;
}

static void on_init(Game* game) {
    Engine* engine = game->engine;
    engine->show_fps = !engine->headless;

    g_scene = create_scene();
    game_set_scene(game, g_scene);

    PhysicsConfig pc = physics_default_config();
    PhysicsWorld* physics = create_physics_world(&pc);
    game_set_physics_world(game, physics);
    EntityManager* em = create_entity_manager(game);
    game_set_entity_manager(game, em);

    Kit kit;
    kit_init(&kit, g_scene, em, physics);
    mats_register(&kit, engine, g_scene);
    house_build(&kit);
    interior_build(&kit);
    hearth_build(&kit);
    kitchen_build(&kit, (unsigned int)g_args.seed);
    lights_build(&g_lights, &kit, engine, g_scene, (unsigned int)g_args.seed, !g_args.no_flicker,
                 g_args.flashlight);
    street_build(&kit, g_scene, (unsigned int)g_args.seed, !g_args.day);
    clock_build(&kit);
    study_build(&kit, g_scene, (unsigned int)g_args.seed);
    kit_finish(&kit, "world");
    printf("silent: %d colliders, %d vertices in %d meshes and %d shadow cells, %d of %d drip "
           "lines, %d candles\n",
           kit.collider_count, kit.vertex_count, kit.mesh_count, kit.shadow_cell_count,
           kit.drip_count, RAIN_DRIP_MAX, kit.wick_count);
    if (!g_args.no_candles) {
        g_scene->fire = create_fire_system();
        candles_light(g_scene->fire, g_scene, &kit, !g_args.no_candle_shadows);
    }
    g_door_hung = house_front_door(&g_door, engine, g_scene, em, physics);
    prompt_start(&g_prompt, engine);
    if (!g_args.no_cat) {
        CatDesc cat = {.at = g_args.cat_at,
                       .clip = g_args.cat_clip[0] ? g_args.cat_clip : NULL,
                       .clip_seconds = g_args.cat_clip_seconds,
                       .go = g_args.cat_go[0] ? g_args.cat_go : NULL,
                       .gait = g_args.cat_gait,
                       .eyeshine = !g_args.no_eyeshine};
        glm_vec3_copy(g_args.cat_fur, cat.fur);
        glm_vec3_copy(g_args.cat_eyes, cat.eyes);
        cat_create(&g_cat, &cat, game, physics);
    }

    // Sound: the clock's beat, the tubes' buzz, the fridge and the wind, each
    // heard from where it is. Headless, the system opens no device, so a
    // capture is unchanged by it.
    AudioSystem* audio = create_audio_system(engine->headless);
    if (audio) {
        game_set_audio_system(game, audio);
        if (g_args.mute)
            audio_set_bus_volume(audio, AUDIO_BUS_MASTER, 0.0f);
    }
    clock_start(&g_clock, engine, g_scene, audio);
    lights_start_audio(&g_lights, audio);
    const vec3 spawn_eye = {SPAWN_FEET[0], SPAWN_FEET[1] + PLAYER_EYE_HEIGHT, SPAWN_FEET[2]};
    sounds_start(&g_sounds, audio, spawn_eye);
    cat_voice_start(&g_voice, &g_cat, audio, g_args.cat_say);

    // Before the sky: its reflections are baked through the fog set here.
    build_post(engine, !g_args.day, !g_args.no_grade);
    build_sky(engine);

    if (!g_args.no_wind) {
        Wind* wind = create_wind("street");
        if (wind) {
            glm_vec3_copy((vec3){0.0f, 0.0f, 1.0f}, wind->direction);
            wind->air_speed = WIND_AIR_SPEED;
            wind->gust_frequency = WIND_GUST_FREQUENCY;
            wind->gust_amount = WIND_GUST_AMOUNT;
            scene_set_wind(g_scene, wind);
        }
    }

    // Already soaked: the game opens in the middle of the rain, not at its start.
    if (g_args.rain_mmh > 0.0f) {
        g_scene->rain = create_rain();
        if (g_scene->rain) {
            g_scene->rain->rate_mmh = g_args.rain_mmh;
            g_scene->rain->streak_brightness = RAIN_STREAK_BRIGHTNESS;
            g_scene->rain->streak_radius = RAIN_STREAK_RADIUS;
            g_scene->rain->streak_count = RAIN_STREAK_COUNT;
            g_scene->rain->fall_scale = RAIN_FALL_SCALE;
            g_scene->rain->streak_sheen = RAIN_STREAK_SHEEN;
            g_scene->rain->wet_darkening = RAIN_WET_DARKENING;
            g_scene->rain->splash_size = RAIN_SPLASH_SIZE;
            g_scene->rain->glass_drop_size = RAIN_GLASS_DROP_SIZE;
            g_scene->rain->drip_brightness = RAIN_DRIP_BRIGHTNESS;
            g_scene->rain->puddle_relief = g_args.no_relief ? 0.0f : RAIN_PUDDLE_RELIEF;
            rain_set_drip_lines(g_scene->rain, kit.drips, kit.drip_count);
            rain_settle(g_scene->rain);
        }
    }
    rain_bed_start(&g_rain_bed, audio, g_scene->rain);

    ShadowSystem* ss = g_scene->shadow_system;
    if (ss) {
        ss->enabled = true;
        ss->ortho_size = 60.0f;
        ss->near_plane = 0.1f;
        ss->far_plane = 200.0f;
        ss->shadow_distance = 40.0f;
        ss->cascade_count = 2;
        ss->pcss_enabled = true;
        ss->tile_views = g_args.tile_views;
    }

    CameraDesc cam = {.position = {SPAWN_FEET[0], SPAWN_FEET[1] + PLAYER_EYE_HEIGHT, SPAWN_FEET[2]},
                      .look_at = {SPAWN_FEET[0], SPAWN_FEET[1] + PLAYER_EYE_HEIGHT, 0.0f},
                      .fov = glm_rad(g_args.fov_deg > 0.0f ? g_args.fov_deg : 68.0f),
                      .near = 0.05f,
                      .far = 250.0f};
    engine_set_camera(engine, create_camera(&cam));

    player_init(&g_player, game, physics, em, SPAWN_FEET, SPAWN_YAW);
    physics_world_optimize(physics);
    // Sent somewhere or holding a clip from the command line, the cat has no mind of its own.
    if (g_cat.entity && !g_args.cat_go[0] && !g_args.cat_clip[0])
        cat_mind_create(&g_mind, &g_cat, game, &g_player, g_args.cat_seed, g_args.cat_blind,
                        g_args.cat_doing);

    // Pinned at night: a meter would open the dark back up, which is the one
    // thing this place must not do. By day the meter maps what it reads to
    // the key on its own, so the camera is 1, and it is floored where that
    // key lands on the night's pin -- a dim room never opens past the night.
    Exposure* ex = &engine->exposure;
    if (g_args.day) {
        ex->key = DAY_METER_KEY;
        ex->meter_min_log2 = log2f(ex->key / EXPOSURE_NIGHT);
    } else {
        ex->automatic = false;
        ex->multiplier = EXPOSURE_NIGHT;
    }
    ex->probe = g_args.exposure_probe;
}

static void on_update(Game* game, double dt) {
    // The cat first, while the player's body still holds the last step's solved velocity, which
    // the player's own update replaces with the one it asks for.
    vec3 feet = {0.0f, 0.0f, 0.0f};
    player_feet(&g_player, feet);
    cat_step(&g_cat, feet, (float)dt);
    // What it senses is decided here, and what it does about it by its brain after this hook.
    cat_mind_sense(&g_mind, g_args.rain_mmh / CAT_HEAVY_RAIN, (float)dt);
    player_update(&g_player, game, dt);
    if (g_door_hung)
        door_update(&g_door, (float)dt);
    if (g_args.trace_cat) {
        static int step;
        if (step++ % 30 == 0) {
            cat_trace(&g_cat, step - 1);
            cat_mind_trace(&g_mind);
        }
    }
    // Where the capsule is: the camera rides it, so from inside the frame a
    // player stopped by a wall and one walking on the spot look the same.
    if (g_args.trace_player && g_player.entity) {
        static int step;
        if (step++ % 30 == 0) {
            const float* p = g_player.entity->position;
            printf("player step %4d pos %7.3f %7.3f %7.3f\n", step - 1, (double)p[0], (double)p[1],
                   (double)p[2]);
        }
    }
}

// The mix up to the sim clock's now, pulled from the offline device: as many
// frames as the clock has run since the last pull, so the sound and the
// picture keep step at any frame rate.
static void dump_audio(Game* game) {
    if (!g_args.audio_dump || !game->audio)
        return;
    const size_t due = (size_t)llround(game->time * DUMP_RATE);
    if (due <= g_dump_frames)
        return;
    const size_t want = due - g_dump_frames;
    if (g_dump_frames + want > g_dump_cap) {
        const size_t cap = 2 * (g_dump_frames + want);
        float* grown = realloc(g_dump, cap * 2 * sizeof(float));
        if (!grown)
            return;
        g_dump = grown;
        g_dump_cap = cap;
    }
    g_dump_frames += audio_system_read_pcm(game->audio, g_dump + 2 * g_dump_frames, want);
}

static bool write_dump(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f)
        return false;
    const uint32_t bytes = (uint32_t)(g_dump_frames * 2 * sizeof(int16_t));
    const uint32_t rate = DUMP_RATE, byte_rate = DUMP_RATE * 4, fmt_size = 16, riff = 36 + bytes;
    const uint16_t pcm = 1, channels = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_size, 4, 1, f);
    fwrite(&pcm, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&bytes, 4, 1, f);
    for (size_t i = 0; i < 2 * g_dump_frames; i++) {
        const float v = glm_clamp(g_dump[i], -1.0f, 1.0f);
        const int16_t s = (int16_t)lrintf(v * 32767.0f);
        fwrite(&s, 2, 1, f);
    }
    return fclose(f) == 0;
}

static void on_pre_render(Game* game, double alpha) {
    (void)alpha;
    Engine* engine = game->engine;
    if (input_action_pressed(&game->input, "toggle_gui"))
        engine->show_gui = !engine->show_gui;
    const bool pinned = g_args.cam_eye_set && g_args.cam_target_set;
    if (g_args.cat_cam && g_cat.entity) {
        // Over the cat's shoulder, looking where it is going.
        const float* c = g_cat.entity->position;
        vec3 target = {c[0], c[1] + 0.05f, c[2]};
        vec3 behind = {c[0] - 1.1f * sinf(g_cat.yaw), c[1] + 0.6f, c[2] - 1.1f * cosf(g_cat.yaw)};
        player_pre_render(&g_player, game, &behind, &target);
    } else {
        player_pre_render(&g_player, game, pinned ? &g_args.cam_eye : NULL,
                          pinned ? &g_args.cam_target : NULL);
    }

    if (input_action_pressed(&game->input, "flashlight"))
        lights_toggle_flashlight(&g_lights);
    vec3 eye = {0.0f, 0.0f, 0.0f}, forward = {0.0f, 0.0f, -1.0f};
    player_eye(&g_player, eye, forward);

    // The door, if the player is looking at it, says what the action key would do to it, and
    // the key does it.
    Door* door =
        g_door_hung && door_reach_distance(&g_door, eye, forward, DOOR_REACH, DOOR_CONE) < FLT_MAX
            ? &g_door
            : NULL;
    if (door && input_action_pressed(&game->input, "interact"))
        door_toggle(door);
    prompt_show(&g_prompt, !door                  ? NULL
                           : door_will_open(door) ? "E   Open door"
                                                  : "E   Close door");
    sounds_update(&g_sounds, eye, (float)game->sim_clock.delta);
    const float hearing = sounds_indoor_gain(&g_sounds);
    lights_update(&g_lights, g_scene, game->time, (float)game->sim_clock.delta, eye, forward,
                  hearing);
    cat_mind_frame(&g_mind, game->time);
    cat_debug_draw(&g_cat, &g_mind, engine);
    cat_update(&g_cat, game, g_scene, &g_lights, eye, (float)game->sim_clock.delta);
    cat_voice_update(&g_voice, &g_sounds, eye, cat_mind_at_ease(&g_mind),
                     (float)game->sim_clock.delta);
    clock_update(&g_clock, game->time, hearing);
    rain_bed_update(&g_rain_bed, g_scene->rain, g_scene->shadow_system, eye,
                    (float)game->sim_clock.delta);
    dump_audio(game);

    // The volume goes in on the third frame, not at load. The tubes' panels are
    // derived during the first frame's draw and only cast from the next, and a
    // volume's FIRST sweep is the only one taken at full weight -- a re-arm
    // blends into what is already there -- so it has to see the lit room. The
    // reflection probes go in with it, since they are captured once it has
    // converged and a set installed with no volume would be captured unlit.
    if (engine->total_frames == 2 && !g_scene->gi && !g_args.no_gi) {
        build_gi();
        build_probes();
    }
    const bool lit = engine->total_frames > 2 && !gi_world_pending(g_scene->gi);

    // Black until the volume's opening sweep has landed, then up. That sweep
    // is one long frame, so without this the window holds the room unlit by
    // its own bounce light for its whole length and then jumps. A volume that
    // could not be built lets the view up rather than holding it dark forever.
    // The fade rides the grade's gain, after the tonemap, so the exposure and
    // the day's meter never see it. Each frame's step is capped because the
    // frame after the sweep carries the sweep's whole length.
    if (lit)
        g_fade_seconds += fminf((float)game->sim_clock.delta, 1.0f / 30.0f);
    if (engine->postfx)
        glm_vec3_fill(engine->postfx->grade_gain,
                      glm_smoothstep(0.0f, FADE_IN_SECONDS, g_fade_seconds));
}

// Before the engine goes: the prompt draws through its overlay hook.
static void on_shutdown(Game* game) {
    if (game && game->engine)
        profiler_report(game->engine->profiler);
    if (g_args.tiles_probe && g_scene)
        shadow_tiles_probe(g_scene->shadow_system, g_scene);
    prompt_free(&g_prompt);
    cat_free(&g_cat);
}

static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("  -x, --headless          Hidden window, for capture\n");
    printf("  -f, --frames N          Exit after N frames\n");
    printf("  -S, --screenshot PATH   Write the final frame as a binary PPM\n");
    printf("      --screenshot-every N  Also write every Nth frame\n");
    printf("  -W, --width N           Window width (default %d)\n", DEFAULT_WIDTH);
    printf("  -H, --height N          Window height (default %d)\n", DEFAULT_HEIGHT);
    printf("      --seed N            Clutter and street seed\n");
    printf("      --day               An overcast day in the fog instead of night\n");
    printf("      --no-taa            No temporal AA, and so no upscale: full resolution, raw "
           "edges\n");
    printf("      --no-grade          Without the green-grey colour grade\n");
    printf("      --local-exposure    An exposure per pixel on top of the camera's\n");
    printf("      --no-local-exposure Without it\n");
    printf("      --le-highlights F   Its contrast above middle grey (1 = none)\n");
    printf("      --le-shadows F      And below it\n");
    printf("      --le-blend F        Its share of the base from the blurred luminance\n");
    printf("      --le-probe          Print the frame's luminance percentiles, in stops from\n"
           "                          middle grey, while it is on\n");
    printf("      --negative-probe    Print how many pixels reach the tonemap below zero, each\n"
           "                          frame\n");
    printf("      --exposure-probe    Print what the meter decided, every frame it decides\n");
    printf("      --no-history-rescale  The fog and temporal histories keep the exposure they\n"
           "                          were written at, so stepping outdoors settles slowly\n");
    printf("      --render-scale F    Render at F of the window and upscale (0.5-1, default\n");
    printf("                          %.1f): the softer frame of the consoles it imitates\n",
           (double)DEFAULT_RENDER_SCALE);
    printf("      --msaa N            MSAA samples\n");
    printf("      --cam-eye x,y,z     Pin the camera (a framing that can be taken twice)\n");
    printf("      --cam-target x,y,z  What the pinned camera looks at\n");
    printf("      --fov D             Vertical field of view, degrees (default 68)\n");
    printf("      --pad-script PATH   Replay a scripted pad on slot 0 (see input.h)\n");
    printf("      --trace-player      Print the player's position every 30 steps\n");
    printf("      --no-flicker        Keep the failing ceiling tube steady\n");
    printf("      --flashlight        Start with the flashlight on (F toggles it)\n");
    printf("      --mute              Without sound\n");
    printf("      --audio-dump PATH   Headless: write what the listener hears as a WAV\n");
    printf("      --rain MM           Rain rate in mm/h (default %.0f)\n",
           (double)DEFAULT_RAIN_MMH);
    printf("      --no-rain           A dry night\n");
    printf("      --no-wind           Still air: the rain falls straight\n");
    printf("      --no-relief         Puddles stand where the noise puts them, not in the\n"
           "                          ground's own lows\n");
    printf("      --no-candles        The candles stand unlit\n");
    printf("      --no-candle-shadows The candles light through walls\n");
    printf("      --no-gi             No bounce light: no GI volume, and no reflection probes,\n"
           "                          which are captured from its light\n");
    printf("      --profiler          Per-pass timing and submission counts, at exit\n");
    printf("      --tile-views N      Shade every cached light from N views over its body\n"
           "                          rather than 8; 1 is its centre alone\n");
    printf("      --tiles-probe       The cached shadow tiles and each light's block, at exit\n");
    printf("      --no-cat            Without the cat\n");
    printf("      --cat-fur RRGGBB    The cat's coat, as sRGB hex (default 262424)\n");
    printf("      --cat-eyes RRGGBB   Its eyes (default E8B923)\n");
    printf("      --cat-at PLACE      Where it is: %s\n", cat_place_list());
    printf("      --cat-clip NAME[@S] Hold that clip there, playing or S seconds in\n");
    printf("      --cat-goto PLACE[:trot|:run]  Send it there once it is in the house, and\n"
           "                          nowhere else: it has no mind of its own then\n");
    printf("      --cat-activity NAME Start it on one of: %s\n", cat_mind_activities());
    printf("      --cat-seed N        Its mind's seed (default 1)\n");
    printf("      --cat-blind         It neither sees nor hears you\n");
    printf("      --cat-say           It makes every sound it has in turn, and purrs, to be\n"
           "                          listened to through --audio-dump\n");
    printf("      --cat-cam           The camera follows the cat\n");
    printf("      --trace-cat         Print what the cat is doing every 30 steps\n");
    printf("      --no-eyeshine       Its eyes do not throw the flashlight back\n");
    printf("  In the window: click to capture the mouse, Tab to release it. WASD\n");
    printf("  walks, Shift hurries, the arrows or the mouse look, E opens and shuts\n");
    printf("  a door you are facing, F the flashlight, G shows the GUI and frees the\n");
    printf("  mouse for it while it is open.\n");
    printf("  -h, --help              This message\n");
}

// "RRGGBB", with or without a leading '#', as sRGB 0..1.
static bool parse_hex(const char* s, vec3 out) {
    unsigned int rgb;
    if (*s == '#')
        s++;
    if (strlen(s) != 6 || sscanf(s, "%x", &rgb) != 1)
        return false;
    for (int i = 0; i < 3; i++)
        out[i] = (float)((rgb >> (16 - 8 * i)) & 0xffu) / 255.0f;
    return true;
}

static bool parse_args(int argc, char** argv, SilentArgs* a) {
    memset(a, 0, sizeof(*a));
    a->width = DEFAULT_WIDTH;
    a->height = DEFAULT_HEIGHT;
    a->seed = 7;
    a->render_scale = DEFAULT_RENDER_SCALE;
    a->rain_mmh = DEFAULT_RAIN_MMH;
    parse_hex("262424", a->cat_fur);
    parse_hex("E8B923", a->cat_eyes);
    a->cat_clip_seconds = -1.0f;
    a->cat_seed = 1;
    a->local_exposure = -1;
    a->le_highlights = -1.0f;
    a->le_shadows = -1.0f;
    a->le_blend = -1.0f;
    for (int i = 1; i < argc; i++) {
        const char* s = argv[i];
        const bool has_next = i + 1 < argc;
        if (!strcmp(s, "-x") || !strcmp(s, "--headless")) {
            a->headless = true;
        } else if ((!strcmp(s, "-f") || !strcmp(s, "--frames")) && has_next) {
            a->frames = atoi(argv[++i]);
        } else if ((!strcmp(s, "-S") || !strcmp(s, "--screenshot")) && has_next) {
            a->screenshot = argv[++i];
        } else if (!strcmp(s, "--screenshot-every") && has_next) {
            a->screenshot_every = atoi(argv[++i]);
        } else if ((!strcmp(s, "-W") || !strcmp(s, "--width")) && has_next) {
            a->width = atoi(argv[++i]);
        } else if ((!strcmp(s, "-H") || !strcmp(s, "--height")) && has_next) {
            a->height = atoi(argv[++i]);
        } else if (!strcmp(s, "--seed") && has_next) {
            a->seed = atoi(argv[++i]);
        } else if (!strcmp(s, "--day")) {
            a->day = true;
        } else if (!strcmp(s, "--no-taa")) {
            a->no_taa = true;
        } else if (!strcmp(s, "--no-grade")) {
            a->no_grade = true;
        } else if (!strcmp(s, "--local-exposure")) {
            a->local_exposure = 1;
        } else if (!strcmp(s, "--no-local-exposure")) {
            a->local_exposure = 0;
        } else if (!strcmp(s, "--le-highlights") && has_next) {
            a->le_highlights = (float)atof(argv[++i]);
        } else if (!strcmp(s, "--le-shadows") && has_next) {
            a->le_shadows = (float)atof(argv[++i]);
        } else if (!strcmp(s, "--le-blend") && has_next) {
            a->le_blend = (float)atof(argv[++i]);
        } else if (!strcmp(s, "--le-probe")) {
            a->le_probe = true;
        } else if (!strcmp(s, "--negative-probe")) {
            a->negative_probe = true;
        } else if (!strcmp(s, "--exposure-probe")) {
            a->exposure_probe = true;
        } else if (!strcmp(s, "--no-history-rescale")) {
            a->no_history_rescale = true;
        } else if (!strcmp(s, "--render-scale") && has_next) {
            a->render_scale = (float)atof(argv[++i]);
        } else if (!strcmp(s, "--msaa") && has_next) {
            a->msaa = atoi(argv[++i]);
        } else if (!strcmp(s, "--cam-eye") && has_next) {
            a->cam_eye_set =
                sscanf(argv[++i], "%f,%f,%f", &a->cam_eye[0], &a->cam_eye[1], &a->cam_eye[2]) == 3;
        } else if (!strcmp(s, "--cam-target") && has_next) {
            a->cam_target_set = sscanf(argv[++i], "%f,%f,%f", &a->cam_target[0], &a->cam_target[1],
                                       &a->cam_target[2]) == 3;
        } else if (!strcmp(s, "--fov") && has_next) {
            a->fov_deg = (float)atof(argv[++i]);
        } else if (!strcmp(s, "--pad-script") && has_next) {
            a->pad_script = argv[++i];
        } else if (!strcmp(s, "--trace-player")) {
            a->trace_player = true;
        } else if (!strcmp(s, "--no-flicker")) {
            a->no_flicker = true;
        } else if (!strcmp(s, "--flashlight")) {
            a->flashlight = true;
        } else if (!strcmp(s, "--mute")) {
            a->mute = true;
        } else if (!strcmp(s, "--audio-dump") && has_next) {
            a->audio_dump = argv[++i];
        } else if (!strcmp(s, "--rain") && has_next) {
            a->rain_mmh = fmaxf(0.0f, (float)atof(argv[++i]));
        } else if (!strcmp(s, "--no-rain")) {
            a->rain_mmh = 0.0f;
        } else if (!strcmp(s, "--no-wind")) {
            a->no_wind = true;
        } else if (!strcmp(s, "--no-relief")) {
            a->no_relief = true;
        } else if (!strcmp(s, "--no-candles")) {
            a->no_candles = true;
        } else if (!strcmp(s, "--no-candle-shadows")) {
            a->no_candle_shadows = true;
        } else if (!strcmp(s, "--no-gi")) {
            a->no_gi = true;
        } else if (!strcmp(s, "--tile-views") && has_next) {
            a->tile_views = atoi(argv[++i]);
        } else if (!strcmp(s, "--tiles-probe")) {
            a->tiles_probe = true;
        } else if (!strcmp(s, "--profiler")) {
            a->profiler = true;
        } else if (!strcmp(s, "--no-cat")) {
            a->no_cat = true;
        } else if (!strcmp(s, "--cat-fur") && has_next) {
            if (!parse_hex(argv[++i], a->cat_fur))
                fprintf(stderr, "silent: --cat-fur wants RRGGBB; keeping the default\n");
        } else if (!strcmp(s, "--cat-eyes") && has_next) {
            if (!parse_hex(argv[++i], a->cat_eyes))
                fprintf(stderr, "silent: --cat-eyes wants RRGGBB; keeping the default\n");
        } else if (!strcmp(s, "--cat-at") && has_next) {
            a->cat_at = argv[++i];
        } else if (!strcmp(s, "--cat-clip") && has_next) {
            snprintf(a->cat_clip, sizeof(a->cat_clip), "%s", argv[++i]);
            char* at = strchr(a->cat_clip, '@');
            if (at) {
                *at = '\0';
                a->cat_clip_seconds = fmaxf(0.0f, (float)atof(at + 1));
            }
        } else if (!strcmp(s, "--no-eyeshine")) {
            a->no_eyeshine = true;
        } else if (!strcmp(s, "--cat-goto") && has_next) {
            snprintf(a->cat_go, sizeof(a->cat_go), "%s", argv[++i]);
            char* how = strchr(a->cat_go, ':');
            if (how) {
                *how = '\0';
                a->cat_gait = !strcmp(how + 1, "trot")  ? CAT_TROT
                              : !strcmp(how + 1, "run") ? CAT_RUN
                                                        : CAT_WALK;
            }
        } else if (!strcmp(s, "--cat-activity") && has_next) {
            a->cat_doing = argv[++i];
        } else if (!strcmp(s, "--cat-seed") && has_next) {
            a->cat_seed = (unsigned int)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(s, "--cat-blind")) {
            a->cat_blind = true;
        } else if (!strcmp(s, "--cat-say")) {
            a->cat_say = true;
        } else if (!strcmp(s, "--cat-cam")) {
            a->cat_cam = true;
        } else if (!strcmp(s, "--trace-cat")) {
            a->trace_cat = true;
        } else if (!strcmp(s, "-h") || !strcmp(s, "--help")) {
            print_usage(argv[0]);
            return false;
        } else {
            fprintf(stderr, "silent: unknown or incomplete option: %s\n", s);
            print_usage(argv[0]);
            return false;
        }
    }
    if (a->cam_eye_set != a->cam_target_set)
        fprintf(stderr, "silent: --cam-eye and --cam-target go together; ignoring the pose\n");
    if (a->audio_dump && !a->headless) {
        fprintf(stderr, "silent: --audio-dump needs --headless, which renders sound offline; "
                        "ignoring it\n");
        a->audio_dump = NULL;
    }
    return true;
}

int main(int argc, char** argv) {
    if (!parse_args(argc, argv, &g_args))
        return 1;

    GameConfig config = {.engine = {.title = "silent",
                                    .width = g_args.width,
                                    .height = g_args.height,
                                    .headless = g_args.headless,
                                    .profiler = g_args.profiler}};
    // One sample under TAA, headless as in the window, so a screenshot is what
    // a player sees. Not MSAA: in fog this dense the fog composite takes ONE
    // depth for a multisampled edge pixel, and where half its samples are sky
    // that depth is too near -- every silhouette in the street comes out
    // traced in a dark, unfogged line.
    config.engine.msaa_samples = 1;
    config.engine.taa = !g_args.no_taa;
    // And jittered, or headless TAA integrates one sample position forever:
    // no antialiasing, and an upscale the engine refuses. The jitter follows
    // the frame index, so a headless run still repeats itself exactly.
    config.engine.headless_jitter = config.engine.taa;
    // The upscale rides on TAA, so a scale without it is refused by the engine.
    if (g_args.render_scale > 0.0f && !g_args.no_taa)
        config.engine.render_scale = g_args.render_scale;
    if (g_args.msaa > 0)
        config.engine.msaa_samples = g_args.msaa;

    Game* game = create_game(&config);
    if (!game) {
        fprintf(stderr, "silent: failed to create the game\n");
        return 1;
    }
    game->engine->exit_after_frames = g_args.frames;
    engine_set_screenshot_path(game->engine, g_args.screenshot);
    game->engine->screenshot_every = g_args.screenshot_every;

    input_bind(&game->input, ACTIONS, sizeof(ACTIONS) / sizeof(ACTIONS[0]));
    if (g_args.pad_script && !input_set_pad_script(&game->input, g_args.pad_script)) {
        free_game(game);
        return 1;
    }

    game_set_init(game, on_init);
    game_set_update(game, on_update);
    game_set_pre_render(game, on_pre_render);
    game_set_shutdown(game, on_shutdown);
    game_run(game);
    if (g_args.audio_dump) {
        if (write_dump(g_args.audio_dump))
            printf("silent: %.1f s of sound in %s\n", (double)g_dump_frames / DUMP_RATE,
                   g_args.audio_dump);
        else
            fprintf(stderr, "silent: cannot write %s\n", g_args.audio_dump);
        free(g_dump);
    }
    free_game(game);
    return 0;
}
