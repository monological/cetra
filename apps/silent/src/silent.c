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

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "cetra/camera.h"
#include "cetra/engine.h"
#include "cetra/gi_volume.h"
#include "cetra/ibl.h"
#include "cetra/light.h"
#include "cetra/loading_screen.h"
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

#include "backpack.h"
#include "backpack_menu.h"
#include "basement.h"
#include "cabin.h"
#include "candles.h"
#include "cat.h"
#include "cat_brain.h"
#include "cat_debug.h"
#include "cat_voice.h"
#include "clock.h"
#include "crossroads.h"
#include "door.h"
#include "fences.h"
#include "footsteps.h"
#include "fridge_map.h"
#include "grounds.h"
#include "haunt.h"
#include "hearth.h"
#include "hill.h"
#include "home.h"
#include "house.h"
#include "hud.h"
#include "interior.h"
#include "kit.h"
#include "kitchen.h"
#include "lake.h"
#include "land.h"
#include "layout.h"
#include "lights.h"
#include "mansion.h"
#include "map_screen.h"
#include "mats.h"
#include "pause_menu.h"
#include "player.h"
#include "rain_bed.h"
#include "save_game.h"
#include "settings_menu.h"
#include "sounds.h"
#include "story.h"
#include "street.h"
#include "study.h"
#include "terrace.h"
#include "title.h"
#include "town_map.h"
#include "town_plan.h"
#include "trees.h"
#include "tv.h"
#include "woods.h"

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
// is in, and to go down to it from the title once a game is chosen.
#define FADE_IN_SECONDS  1.0f
#define FADE_OUT_SECONDS 0.6f
// The engine's capture budget while the view is still black (spec 13.32): nothing on screen
// is waiting, so the captures take most of each frame and the wait is short, while the window
// still answers a few times a second. Once the view is up the engine's own budget stands.
#define LOADING_CAPTURE_MS 100.0f
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
    bool crt;
    int local_exposure;      // 1 on, 0 off; -1 = this mode's own choice
    float le_highlights;     // the local exposure's highlight contrast; below 0 = this mode's own
    float le_shadows;        // and below it
    float le_blend;          // and its blurred-luminance blend
    bool le_probe;           // print the frame's luminance percentiles it is asked to fit
    bool negative_probe;     // print how much of the frame reaches the tonemap below zero
    bool exposure_probe;     // print what the meter decided, every frame it decides
    bool no_history_rescale; // histories keep the exposure they were written at (A/B)
    int fog_miss_samples;    // samples a fog cell with no history takes; 0 = the engine's
    float render_scale;
    int msaa;
    bool cam_eye_set, cam_target_set;
    vec3 cam_eye, cam_target;
    bool player_at_set;
    vec3 player_at; // where the player starts in plan, x and z, and the yaw, in degrees from +z
    float fov_deg;
    const char* pad_script;
    bool trace_player;
    bool trace_gait;
    bool no_interp;
    bool no_gait;
    float bob;   // the head's bob, as a share of the gait's own
    float climb; // the pace up and down a flight, as a share of the gait's own
    bool no_footsteps;
    bool footstep_say; // every footstep take in turn, for listening in an audio dump
    bool crouch;       // start crouched
    bool no_flicker;
    bool no_static;         // the living room's set showing nothing, a faint glow and no hiss
    bool haunt;             // the haunting begins as the game is played (spec 13.52)
    float haunt_at;         // and this many seconds of it already gone by
    bool trace_haunt;       // print each of its beats as it comes
    bool no_loading_screen; // load under a black window, with no engine screen over it
    bool loading_screen;    // the screen headless too, where it is otherwise left out
    bool startup_ms;        // print where loading's time goes, a startup-ms row a step
    bool flashlight;
    bool backpack;           // the backpack already taken, and the flashlight in it
    bool map;                // the town map already in it, and the fridge door bare
    bool key;                // the front door's key in it, and the workbench bare (spec 13.53)
    bool open_map;           // the map's screen open at the start, over the backpack's when that is
    bool found[PLACE_COUNT]; // places found before the run starts, their marks not yet written
    ItemId open_backpack;    // the backpack's screen open on it at the start, or ITEM_NONE
    bool mute;
    float rain_mmh;          // 0 = dry
    bool no_wind;            // still air: the rain falls straight
    bool no_fog;             // clear air: no fog volumes and no haze, to see the layout
    bool no_relief;          // puddles from the noise alone, not the ground's own lows
    bool no_candles;         // the candles stand unlit
    bool no_candle_shadows;  // the candles light through walls, as before spec 13.16
    bool no_gi;              // no bounce light: no GI volume, and no reflection probes from it
    int tile_views;          // views over each cached light's body; 0 = the engine's, 1 = one
    int tile_stores;         // store cells for faces drawn over a copy; -1 = the engine's
    bool tiles_probe;        // print the cached shadow tiles at exit
    float capture_budget_ms; // the engine's capture budget, pinned; below 0 = silent's own
    bool capture_timing;     // each GI volume prints what its sweep cost as it converges
    float gi_cull_pixels;    // the GI world's cull_pixels
    float gi_cell;           // the home's GI cell in metres; 0 = HOME_GI_CELL
    bool profiler;           // per-pass timing and submission counts, reported at exit
    const char* audio_dump;  // headless: write what the listener hears here
    const char* map_export;  // write the town's plan here for tools/make_map.py, and stop
    bool no_woods;           // no trees behind the yards, nor what lies under them
    bool no_cat;
    vec3 cat_fur, cat_eyes; // sRGB
    const char* cat_at;     // a place by name, or NULL for home
    char cat_clip[32];      // a clip by name, or empty for the place's own
    float cat_clip_seconds; // held this far in; below 0 it plays
    bool no_eyeshine;
    char cat_go[32];          // a place it sets off for, or empty to stay
    CatGait cat_gait;         // and how fast
    bool cat_cam;             // the camera follows the cat
    bool trace_cat;           // print what it is doing every 30 steps
    bool cat_say;             // it makes every sound it has, in turn
    unsigned int cat_seed;    // its mind's seed
    bool cat_blind;           // it does not see or hear the player
    const char* cat_doing;    // the activity it starts with, or NULL to choose
    const char* slot;         // the save slot this run saves into, with no title
    const char* load;         // a save slot to start from, with no title
    bool new_game;            // straight into a new game, with no title
    bool no_autosave;         // a game that never saves itself
    bool title;               // the title headless too
    const char* title_screen; // a screen over the title at the start: games, delete
    const char* pause_screen; // the pause menu up once the game is played: menu, settings
} SilentArgs;

static SilentArgs g_args;

/*
 * Whether the run starts somewhere of its own rather than at the title (spec 13.49): from a save,
 * a new game, a slot, or a state the command line gives the world. Such a run saves nowhere unless
 * --slot says where. A flag that changes how the world starts belongs here, or a run with it would
 * go through the title, and a New Game there would keep that state in a player's slot.
 */
static bool starts_elsewhere(const SilentArgs* a) {
    bool found = false;
    for (int k = 0; k < PLACE_COUNT; k++)
        found |= a->found[k];
    return a->load || a->new_game || a->slot || a->player_at_set || found || a->backpack ||
           a->crouch || a->cat_at || a->cat_go[0] || a->cat_clip[0] || a->cat_doing || a->cat_cam ||
           a->haunt || (a->cam_eye_set && a->cam_target_set);
}
static Scene* g_scene;
static Player g_player;
static KitSurfaces g_surfaces; // what the kits' bodies are made of, for the footsteps
static Footsteps g_footsteps;
static Lights g_lights;
static Tv g_tv;
static Clock g_clock;
static Fridge g_fridge;
static Haunt g_haunt;
static Story g_story;
static Lake g_lake;
static RainBed g_rain_bed;
static Sounds g_sounds;
static Cat g_cat;
static CatMind g_mind;
static CatVoice g_voice;

// The street lamps on failing ballasts: the one up the drive and the one at the chasm's lip.
enum { FAILING_DRIVE, FAILING_LIP, FAILING_LAMPS };
static FailingLamp g_failing[FAILING_LAMPS];

// The doors that open: each house's front door, the home's bathroom door (spec 13.25),
// basement door (spec 13.31) and bedroom door (spec 13.40), and the cabin's (spec 13.41).
enum { DOOR_HOME, DOOR_BATH, DOOR_BEDROOM, DOOR_BASEMENT, DOOR_MANSION, DOOR_CABIN, DOORS };
_Static_assert(DOORS <= SAVE_GAME_DOORS, "a save keeps every door");
static Door g_doors[DOORS];
static bool g_door_hung[DOORS];
static Hud g_hud;
static Basement g_basement;
static Backpack g_backpack;
static BackpackMenu g_menu;
static FridgeMap g_fridge_map;
static MapScreen g_map_screen;
// The places the player has found, which go on the map (spec 13.43).
static Finds g_finds;

// How long the prompt names a key once what it opens is had: Tab for the backpack, M for the map.
#define HINT_SECONDS 5.0f
#define MAP_HINT     "M   Map"

// --audio-dump: the offline mix, pulled a frame's worth at a time so it keeps
// step with the sim clock, as interleaved stereo at the engine's rate.
#define DUMP_RATE 48000
static float* g_dump;
static size_t g_dump_frames, g_dump_cap;
static float g_play_capture_ms; // the capture budget once loaded

/*
 * Where the run is (spec 13.49). It loads under the loading screen with the lighting brought in
 * round the first view, comes up on the title over the street when it has one, goes black while
 * the game chosen there is put in place and the lighting round the player comes in, and is played.
 * A run with no title -- headless, or started from a state on the command line -- goes from
 * loading straight to playing. Quit to Title (spec 13.50) goes black, puts the world back to its
 * start, and loads again round the title's view.
 */
typedef enum {
    RUN_LOADING,
    RUN_TITLE,
    RUN_LEAVING,
    RUN_ENTERING,
    RUN_PLAYING,
    RUN_RETURNING
} RunState;
static RunState g_run;
static Title g_title;
static PauseMenu g_pause;
static SettingsMenu g_settings;
static float g_fade; // 0 black .. 1 up, eased by the frame

/*
 * Nothing is played before the game is: its sim is held, so a New Game is the world as it was
 * built however long the title stood, and a load is put over that. The world's look goes on by
 * this clock meanwhile, installed as the engine's render clock -- the rain still falls past the
 * title, the lamps still fail, and the fade still eases. It takes up where the sim was drawn and
 * runs by the frame: a fixed step headless, the wall clock in a window.
 */
static EngineFrameClock g_held_clock;

/*
 * How the game is held each frame: played; FROZEN under the pause menu (spec 13.50), where the
 * whole world stands still on the sim's clock, the rain in the air; or HELD while it is not yet
 * played, the world's look going on by the held clock.
 */
typedef enum { HOLD_PLAYED, HOLD_FROZEN, HOLD_HELD } Hold;

// The frame's own length, however the game is held, as the UI eases by: a fixed step headless, the
// wall clock in a window.
static double frame_dt(const Game* game) {
    const Engine* engine = game->engine;
    return engine->headless ? ENGINE_FIXED_FRAME_DT
                            : fmin(engine->delta_time, game->max_frame_time);
}

// Whether the held clock is the engine's render clock.
static bool looking(const Game* game) {
    return game->engine->render_clock == &g_held_clock;
}

// How far a frame moves what is drawn and eased by it, and the time it is drawn at: the sim's,
// standing still while frozen, or the held clock's while the game is held.
static double look_dt(const Game* game) {
    return looking(game) ? g_held_clock.delta : game->frame_clock.delta;
}

static double look_time(const Game* game) {
    return looking(game) ? g_held_clock.time : game->time;
}

// Before the fixed steps: the sim held or let go, and the render clock that goes with it.
static void hold_game(Game* game, Hold hold) {
    Engine* engine = game->engine;
    if (hold == HOLD_PLAYED)
        game_unpause(game);
    else
        game_pause(game);
    if (hold != HOLD_HELD) {
        engine_set_render_clock(engine, &game->sim_clock);
        return;
    }
    if (!looking(game))
        g_held_clock.time = game->frame_clock.time;
    g_held_clock.delta = frame_dt(game);
    g_held_clock.time += g_held_clock.delta;
    engine_set_render_clock(engine, &g_held_clock);
}

// The spawn: in the kitchen's back corner, looking across the room toward the hall door.
static const vec3 SPAWN_FEET = {4.5f, FLOOR_Y, 13.3f};
static const vec3 SPAWN_LOOK_AT = {0.5f, 1.0f, 10.5f};

// Every action, one table: input_bind takes a single table and borrows it. The
// player reads the move, sprint, crouch, look and cursor rows by name (player.h); the
// flashlight and the GUI are read here.
static const InputAction ACTIONS[] = {
    {"move_x",
     {INPUT_KEY(D, 1), INPUT_KEY(A, -1), INPUT_AXIS(LEFT_X, 1), INPUT_PAD(DPAD_RIGHT, 1),
      INPUT_PAD(DPAD_LEFT, -1)}},
    {"move_y",
     {INPUT_KEY(W, 1), INPUT_KEY(S, -1), INPUT_AXIS(LEFT_Y, -1), INPUT_PAD(DPAD_UP, 1),
      INPUT_PAD(DPAD_DOWN, -1)}},
    {"sprint", {INPUT_KEY(LEFT_SHIFT, 1), INPUT_PAD(LEFT_BUMPER, 1)}},
    // A crouch held, and one toggled (spec 13.48). B is otherwise the screens' back, which reads
    // only while a screen holds the game's input and these read nothing.
    {"crouch", {INPUT_KEY(LEFT_CONTROL, 1)}},
    {"crouch_toggle", {INPUT_KEY(C, 1), INPUT_PAD(B, 1)}},
    {"look_x", {INPUT_AXIS(RIGHT_X, 1), INPUT_KEY(RIGHT, 1), INPUT_KEY(LEFT, -1)}},
    {"look_y", {INPUT_AXIS(RIGHT_Y, -1), INPUT_KEY(UP, 1), INPUT_KEY(DOWN, -1)}},
    {"flashlight", {INPUT_KEY(F, 1), INPUT_PAD(Y, 1)}},
    {"interact", {INPUT_KEY(E, 1), INPUT_PAD(X, 1)}},
    // The pause menu (spec 13.50): a game action, so it reads nothing while a screen holds the
    // game's input, and Escape there is that screen's back.
    {"pause_menu", {INPUT_KEY(ESCAPE, 1), INPUT_PAD(START, 1)}},
    {"toggle_gui", {INPUT_KEY(GRAVE_ACCENT, 1), INPUT_KEY(G, 1)}},
    // The haunting (spec 13.52), begun and ended by hand until a story begins it.
    {"haunt", {INPUT_KEY(K, 1), INPUT_PAD(RIGHT_THUMB, 1)}},
    // The backpack's screen (spec 13.40): UI actions, so they read while it holds the game's input.
    {"backpack", {INPUT_KEY(TAB, 1), INPUT_PAD(BACK, 1)}, true},
    // The map's screen (spec 13.43), and moving and zooming its view.
    {"map", {INPUT_KEY(M, 1), INPUT_PAD(RIGHT_BUMPER, 1)}, true},
    {"map_pan_x",
     {INPUT_KEY(D, 1), INPUT_KEY(A, -1), INPUT_KEY(RIGHT, 1), INPUT_KEY(LEFT, -1),
      INPUT_AXIS(LEFT_X, 1)},
     true},
    {"map_pan_y",
     {INPUT_KEY(S, 1), INPUT_KEY(W, -1), INPUT_KEY(DOWN, 1), INPUT_KEY(UP, -1),
      INPUT_AXIS(LEFT_Y, 1)},
     true},
    {"map_zoom",
     {INPUT_AXIS(RIGHT_TRIGGER, 1), INPUT_AXIS(LEFT_TRIGGER, -1), INPUT_KEY(EQUAL, 1),
      INPUT_KEY(MINUS, -1)},
     true},
    {"menu_back", {INPUT_KEY(ESCAPE, 1), INPUT_PAD(B, 1)}, true},
    {"menu_up", {INPUT_KEY(UP, 1), INPUT_KEY(W, 1), INPUT_PAD(DPAD_UP, 1)}, true},
    {"menu_down", {INPUT_KEY(DOWN, 1), INPUT_KEY(S, 1), INPUT_PAD(DPAD_DOWN, 1)}, true},
    {"menu_left", {INPUT_KEY(LEFT, 1), INPUT_KEY(A, 1), INPUT_PAD(DPAD_LEFT, 1)}, true},
    {"menu_right", {INPUT_KEY(RIGHT, 1), INPUT_KEY(D, 1), INPUT_PAD(DPAD_RIGHT, 1)}, true},
    {"menu_accept",
     {INPUT_KEY(ENTER, 1), INPUT_KEY(SPACE, 1), INPUT_KEY(E, 1), INPUT_PAD(A, 1),
      INPUT_PAD(START, 1)},
     true},
    // A game off the title's list (spec 13.49), after a yes.
    {"menu_delete", {INPUT_KEY(DELETE, 1), INPUT_KEY(BACKSPACE, 1), INPUT_PAD(X, 1)}, true},
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
 * Just outside the grid a query takes the nearest edge probes, fading to the
 * environment's answer a few cells out -- the right kind of answer for the
 * grounds, lit mostly by the moon rather than by what bounces.
 *
 * This is the MANSION's grid (spec 13.25), its plan standing at MANSION_X/Y/Z.
 */
#define GI_CELL  1.21f
#define GI_COLS  11
#define GI_ROWS  7
#define GI_TOP   9.4f
#define GI_CLEAR 0.15f // how near a probe centre may come to a wall's or a slab's face

/*
 * The player's house is plainer and new, so its grid is laid from its bounds and the engine
 * switches off whatever probe lands in a wall (create_gi_volume_spaced): the basement, the
 * ground floor and the porch, 1.25 m a cell. Nothing upstairs is lit or seen. The grid reaches a
 * whole number of cells under the yard, so the ground floor's probes stand where they did before
 * the basement was dug (spec 13.31); its lowest layer lands in the basement's slab and is
 * switched off with the walls. At 1.25 m against 1.0 the house takes 450 probes rather than 792
 * and sweeps about 30% sooner, for a shift in its bounce light of under a code on average
 * (spec 13.42, phase 6).
 */
#define HOME_GI_CELL  1.25f
#define CABIN_GI_CELL 1.0f

// A grid laid over a building's bounds on `cell` metre cells, the probes in its walls switched
// off.
static void build_spaced_gi(const char* name, const vec3 lo, const vec3 hi, float cell) {
    GIVolume* gi = create_gi_volume_spaced(lo, hi, cell);
    if (gi && scene_add_gi_volume(g_scene, gi))
        printf("silent: %s GI %d probes, classified\n", name,
               gi->counts[0] * gi->counts[1] * gi->counts[2]);
}

static void build_home_gi(void) {
    const float cell = g_args.gi_cell > 0.0f ? g_args.gi_cell : HOME_GI_CELL;
    const float under = cell * ceilf(-BASEMENT_Y / cell);
    build_spaced_gi("home", (vec3){HOUSE_X0 - 0.2f, -under, PORCH_Z0 - 0.2f},
                    (vec3){HOUSE_X1 + 0.2f, HOME_CEIL_Y + 0.1f, HOUSE_BACK_Z + 0.2f}, cell);
}

// The cabin's (spec 13.41), over its one room and the porch, from its floor to its ridge.
static void build_cabin_gi(void) {
    build_spaced_gi("cabin", (vec3){CABIN_PORCH_X0 - 0.3f, CABIN_FLOOR_Y - 0.2f, CABIN_Z0 - 0.3f},
                    (vec3){CABIN_X1 + 0.3f, CABIN_RIDGE_Y + 0.1f, CABIN_Z1 + 0.3f}, CABIN_GI_CELL);
}

// The mansion's grid, its plan standing at `origin`.
static void build_gi(const vec3 origin) {
    const vec3 lo = {-7.45f + origin[0], origin[1], 7.58f + origin[2]};
    const vec3 hi = {lo[0] + GI_COLS * GI_CELL, lo[1] + GI_TOP, lo[2] + GI_COLS * GI_CELL};
    GIVolume* gi = create_gi_volume(GI_COLS, GI_ROWS, GI_COLS, lo, hi);
    if (!gi)
        return;
    if (!scene_add_gi_volume(g_scene, gi))
        return;

    // Every centre against the walls and slabs, so a layout change that walks one into a wall
    // says so here rather than as a dark patch.
    int near = 0;
    for (int i = 0; i < gi->counts[0]; i++)
        for (int j = 0; j < gi->counts[1]; j++)
            for (int k = 0; k < gi->counts[2]; k++) {
                const vec3 c = {gi->grid_min[0] + ((float)i + 0.5f) * gi->spacing[0] - origin[0],
                                gi->grid_min[1] + ((float)j + 0.5f) * gi->spacing[1] - origin[1],
                                gi->grid_min[2] + ((float)k + 0.5f) * gi->spacing[2] - origin[2]};
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
 * Reflection probes in the home's kitchen, hall, living room, bathroom and bedroom
 * (spec 13.40), its stairwell down and its basement (spec 13.31), in the mansion's
 * dining room, hall, great hall, study and the study's tower bay (spec 13.25), and
 * in the cabin's one room by the lake (spec 13.41), thirteen in the world: the
 * engine keeps the nearest sixteen resident and captures the mansion's as the drive
 * brings them near, and the cabin's as the track does. Without them every
 * metal and every wet surface indoors reflects the only environment there is,
 * the night sky, and the hood, the sink and the floor go black. The great hall's
 * box goes up to the ridge, since the hall is open to its roof: a roof outside
 * every box reflects the sky.
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
typedef struct ProbeRoom {
    bool mansion; // in the mansion's plan, moved by its origin; else the home's, at the plan's own
    vec3 pos, lo, hi;
} ProbeRoom;

#define PROBE_ROOM_COUNT 13

static void build_probes(void) {
    if (!g_scene->ibl || !g_scene->ibl->precomputed)
        return;
    const ProbeRoom PROBE_ROOMS[PROBE_ROOM_COUNT] = {
        // The home (spec 13.25): its kitchen, the hall the whole depth of the house, the living
        // room, the bathroom and the bedroom (spec 13.40), whose probe stands over its floor
        // between the bed's foot and the chest.
        {false,
         {2.48f, FLOOR_Y + 1.5f, 11.9f},
         {HALL_X1, FLOOR_Y, HOUSE_FRONT_Z + KIT_PANE_HALF},
         {HOUSE_X1 - KIT_PANE_HALF, HOME_CEIL_Y, KITCHEN_BACK_Z}},
        {false,
         {-0.75f, FLOOR_Y + 1.5f, 14.7f},
         {HALL_X0, FLOOR_Y, FRONT_DOOR_Z},
         {HALL_X1, HOME_CEIL_Y, HOUSE_BACK_Z - KIT_PANE_HALF}},
        {false,
         {-3.2f, FLOOR_Y + 1.5f, 13.3f},
         {HOUSE_X0 + KIT_PANE_HALF, FLOOR_Y, HOUSE_FRONT_Z + KIT_PANE_HALF},
         {HALL_X0, HOME_CEIL_Y, HOME_SPLIT_Z}},
        {false,
         {1.3f, FLOOR_Y + 1.4f, 15.2f},
         {HALL_X1, FLOOR_Y, KITCHEN_BACK_Z},
         {BATH_X1, HOME_CEIL_Y, HOME_SPLIT_Z}},
        {false,
         {1.8f, FLOOR_Y + 1.5f, 18.0f},
         {HALL_X1, FLOOR_Y, HOME_SPLIT_Z},
         {HOUSE_X1 - KIT_PANE_HALF, HOME_CEIL_Y, HOUSE_BACK_Z - KIT_PANE_HALF}},
        // Its stairwell, from the basement's slab to the ceiling, and the basement under the
        // whole house (spec 13.31), which overlap only where the stairwell is under the floor.
        // The stairwell's probe is clear of the bulb hanging in it.
        {false,
         {-3.6f, 0.6f, 18.9f},
         {HOUSE_X0 + KIT_PANE_HALF, BASEMENT_Y, STAIRWELL_WALL_Z},
         {HALL_X0, HOME_CEIL_Y, HOUSE_BACK_Z - KIT_PANE_HALF}},
        {false,
         {-1.2f, BASEMENT_Y + 1.25f, 15.3f},
         {CELLAR_X0, BASEMENT_Y, CELLAR_Z0},
         {CELLAR_X1, SUBFLOOR_Y0, CELLAR_Z1}},
        // The mansion: its dining room, its hall, the great hall, the study and the study's bay.
        {true,
         {2.48f, FLOOR_Y + 1.5f, 11.9f},
         {HALL_X1, FLOOR_Y, HOUSE_FRONT_Z + KIT_PANE_HALF},
         {HOUSE_X1 - KIT_PANE_HALF, CEIL_Y, KITCHEN_BACK_Z}},
        {true,
         {-0.75f, FLOOR_Y + 1.5f, 12.0f},
         {HALL_X0, FLOOR_Y, FRONT_DOOR_Z},
         {HALL_X1, CEIL_Y, KITCHEN_BACK_Z}},
        {true,
         {HEARTH_X, FLOOR_Y + 1.8f, 16.6f},
         {HOUSE_X0 + KIT_PANE_HALF, FLOOR_Y, KITCHEN_BACK_Z},
         {HOUSE_X1 - KIT_PANE_HALF, house_roof_y(0.0f), HOUSE_BACK_Z - KIT_PANE_HALF}},
        {true,
         {-3.25f, FLOOR2_Y + 1.6f, 11.9f},
         {HOUSE_X0 + KIT_PANE_HALF, FLOOR2_Y, HOUSE_FRONT_Z + KIT_PANE_HALF},
         {HALL_X0, CEIL2_Y, KITCHEN_BACK_Z}},
        // Over the desk, which stands in the middle of the bay.
        {true,
         {TOWER_X, FLOOR2_Y + 1.8f, TOWER_Z},
         {TOWER_X - TOWER_APOTHEM + KIT_PANE_HALF, FLOOR2_Y,
          TOWER_Z - TOWER_APOTHEM + KIT_PANE_HALF},
         {TOWER_X + TOWER_APOTHEM, TOWER_CEIL_Y, TOWER_Z + TOWER_APOTHEM}},
        // The cabin by the lake (spec 13.41): its one room out to the planes its door and its
        // panes hang in, which are its walls' middles, and up to its eaves. A box to the ridge
        // took in the tin's own top over the room, which then reflected the room rather than the
        // sky, rust-dark beside the overhangs.
        {false,
         {0.5f * (CABIN_X0 + CABIN_X1), CABIN_FLOOR_Y + 1.5f, 0.5f * (CABIN_Z0 + CABIN_Z1) + 0.8f},
         {CABIN_X0, CABIN_FLOOR_Y, CABIN_Z0},
         {CABIN_X1, CABIN_EAVE_Y, CABIN_Z1}},
    };
    ReflectionProbeSet* set = create_reflection_probe_set();
    if (!set)
        return;
    for (int n = 0; n < PROBE_ROOM_COUNT; n++) {
        const ProbeRoom* r = &PROBE_ROOMS[n];
        ReflectionProbe* p = create_reflection_probe();
        if (!p)
            break;
        if (r->mansion) {
            mansion_at(r->pos, p->position);
            mansion_at(r->lo, p->box_min);
            mansion_at(r->hi, p->box_max);
        } else {
            glm_vec3_copy((float*)r->pos, p->position);
            glm_vec3_copy((float*)r->lo, p->box_min);
            glm_vec3_copy((float*)r->hi, p->box_max);
        }
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
        if (!probe_set_add(set, p))
            break;
    }
    if (set->residency.count == PROBE_ROOM_COUNT)
        g_scene->probe_set = set;
    else
        free_reflection_probe_set(set);
}

static void build_post(const Engine* engine, bool night, bool grade) {
    PostFX* fx = engine->postfx;
    if (!fx)
        return;
    postfx_apply_film_look(fx);
    // No film grain; on request, a console's picture on a living-room television (spec 13.28).
    fx->grain_enabled = false;
    fx->crt_enabled = g_args.crt;
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
    // The haze starts at the lake (spec 13.41), the lowest floor there is, or a flashlight beam
    // down the cellar stair or inside the cabin stops dead at the yard's level; its density at
    // the floor is raised by what the falloff takes off over that depth, so every height is as it
    // was.
    fx->fog_enabled = !g_args.no_fog;
    fx->fog_height_falloff = 60.0f;
    fx->fog_floor_y = LAKE_Y;
    fx->fog_density = night ? 0.02f * expf(-LAKE_Y / fx->fog_height_falloff) : 0.0f;
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
    if (g_args.fog_miss_samples > 0)
        fx->fog_history_miss_samples = g_args.fog_miss_samples;
}

// Door `i`, or NULL when it was not hung.
static const Door* hung_door(int i) {
    return g_door_hung[i] ? &g_doors[i] : NULL;
}

// A seam of on_init (spec 13.34): the loading screen moves, and under --startup-ms where loading's
// time goes is said as a startup-ms row, the time since the seam before and the longest the screen
// stood still in it (spec 13.39).
static double g_load_mark, g_settle_start;
static void load_seam(Engine* engine, const char* site) {
    engine_draw_loading_screen(engine);
    const double now = glfwGetTime();
    if (g_args.startup_ms)
        printf("startup-ms site=%s ms=%.1f still=%.1f\n", site, (now - g_load_mark) * 1000.0,
               engine_loading_screen_take_longest_wait(engine) * 1000.0);
    g_load_mark = now;
}

// Under --startup-ms, the frames while the view is held, each the time since the last and the
// longest the screen stood still in it: the first few by name, then the slowest of each; the
// whole wait once the lighting is in, and once the view comes up, both from the end of on_init.
// Headless the view comes up the frame the lighting is in, so that frame says both.
static void trace_settling(Engine* engine, bool lit, bool up) {
    static double worst, worst_still;
    static bool lit_said, up_said;
    if (up_said)
        return;
    const double now = glfwGetTime();
    if (!up) {
        const double still = engine_loading_screen_take_longest_wait(engine) * 1000.0;
        const double ms = (now - g_load_mark) * 1000.0;
        if (engine->total_frames <= 3) {
            printf("startup-ms site=frame%zu ms=%.1f still=%.1f\n", engine->total_frames, ms,
                   still);
        } else {
            worst = fmax(worst, ms);
            worst_still = fmax(worst_still, still);
        }
        g_load_mark = now;
    }
    if (lit && !lit_said) {
        printf("startup-ms site=lit ms=%.1f frames=%zu worst-frame-ms=%.1f worst-still=%.1f\n",
               (now - g_settle_start) * 1000.0, engine->total_frames, worst, worst_still);
        // What the volumes captured, as a digest of their tiles: two runs that photographed the
        // rooms alike print the same one, whatever frames their captures landed in.
        gi_world_probe_print(g_scene->gi, g_scene->lighting_atlas, (int)engine->total_frames);
        lit_said = true;
    }
    if (up) {
        printf("startup-ms site=up ms=%.1f frames=%zu\n", (now - g_settle_start) * 1000.0,
               engine->total_frames);
        up_said = true;
    }
}

// What the captures may take of a frame (spec 13.32): most of it under the loading screen and in
// the black while a game comes up, since nothing on screen is waiting, and the play budget
// otherwise -- unless the run pinned one, and headless, where the frame a capture lands in must
// never be the clock's to decide.
static void capture_budget(Engine* engine) {
    if (g_args.capture_budget_ms >= 0.0f || engine->headless)
        return;
    const bool waiting = g_run == RUN_LOADING || g_run == RUN_ENTERING;
    engine->capture_budget_ms = waiting ? LOADING_CAPTURE_MS : g_play_capture_ms;
}

// Into a game (spec 13.49): the save in `load` put over the world as it was built, or the world as
// built for a new game when it is NULL; then the player's own camera, and from now on the game
// saves itself into `slot`, or nowhere for NULL. False when the save could not be read.
static bool enter_game(Engine* engine, const char* load, const char* slot) {
    if (load && !save_game_load(load))
        return false;
    save_game_begin(g_args.no_autosave ? NULL : slot);
    engine_set_camera_rig(engine, g_player.rig);
    settings_menu_apply(&g_settings);
    return true;
}

// The game played from here on, with the pause menu over it, or the settings over that, the first
// time, when the command line asks for a picture of them (spec 13.50).
static void start_play(void) {
    g_run = RUN_PLAYING;
    const char* screen = g_args.pause_screen;
    g_args.pause_screen = NULL;
    if (!screen)
        return;
    const bool settings = !strcmp(screen, "settings");
    if (settings || !strcmp(screen, "menu"))
        pause_menu_show(&g_pause);
    if (settings)
        settings_menu_show(&g_settings);
}

// The light held at rest while it is captured (spec 13.42), and put back after.
static void rest_lights(bool rest, void* ctx) {
    (void)ctx;
    lights_rest(&g_lights, rest);
    basement_rest(&g_basement, rest);
    tv_rest(&g_tv, rest);
    haunt_rest(&g_haunt, rest);
}

static void on_init(Game* game) {
    Engine* engine = game->engine;
    engine->show_fps = !engine->headless;
    g_load_mark = glfwGetTime();
    // The engine's loading screen while the game loads and its lighting settles (spec 13.34):
    // in a window, and headless only when asked, so a headless run's frames are otherwise the
    // game's from the first.
    if ((!engine->headless || g_args.loading_screen) && !g_args.no_loading_screen)
        engine_show_loading_screen(engine);

    g_scene = create_scene();
    game_set_scene(game, g_scene);

    PhysicsConfig pc = physics_default_config();
    PhysicsWorld* physics = create_physics_world(&pc);
    game_set_physics_world(game, physics);
    EntityManager* em = create_entity_manager(game);
    game_set_entity_manager(game, em);

    load_seam(engine, "scene");

    Kit kit;
    kit_init(&kit, g_scene, em, physics);
    kit.engine = engine;
    kit.surfaces = &g_surfaces;
    mats_register(&kit, engine, g_scene);
    load_seam(engine, "materials");
    // The player's house on the plan's origin (spec 13.25), its living room's television, its
    // basement, its kitchen and its clock.
    home_build(&kit, engine, g_scene);
    load_seam(engine, "home");
    tv_build(&g_tv, &kit, engine, g_scene, !g_args.no_static);
    load_seam(engine, "tv");
    basement_build(&kit, (unsigned int)g_args.seed);
    kitchen_build(&kit, (unsigned int)g_args.seed);
    lights_build(&g_lights, &kit, engine, g_scene, (unsigned int)g_args.seed, !g_args.no_flicker,
                 g_args.flashlight);
    load_seam(engine, "basement-kitchen-lights");
    StreetPlots plots;
    street_build(&kit, g_scene, (unsigned int)g_args.seed, !g_args.day, !g_args.no_fog, &plots);
    load_seam(engine, "street");
    clock_build(&kit);
    // The drive up the hill, the ground over the world, the far side's terrace, the yards' fences,
    // and the crossroads at the chasm's lip (spec 13.35).
    hill_build(&kit);
    load_seam(engine, "clock-hill");
    // The open ground and the chasm's face in a kit of their own, whose shadow cells are six times
    // the size: at the kit's own, the land's 2 m grid made thousands of cells of a few triangles.
    Kit ground;
    kit_init_beside(&ground, &kit, GLM_VEC3_ZERO);
    ground.shadow_cell_scale = 6.0f;
    land_build(&ground);
    // The track down to the lake and the cabin's pad (spec 13.41).
    lake_ground_build(&ground);
    load_seam(engine, "land");
    terrace_build(&kit, plots.far);
    load_seam(engine, "terrace");
    FenceBreaches breaches;
    fences_build(&kit, (unsigned int)g_args.seed, &plots, &breaches);
    load_seam(engine, "fences");
    // The town's plan for its map (spec 13.43), while the street's plots are known: the whole of a
    // run that asks for it.
    if (g_args.map_export)
        exit(town_plan_write(g_args.map_export, &plots, (unsigned int)g_args.seed) ? 0 : 1);
    crossroads_build(&kit, g_scene, !g_args.day, &g_failing[FAILING_LIP]);
    load_seam(engine, "crossroads");
    Trees trees;
    trees_init(&trees, engine, g_scene);
    // In fog no tree is drawn past where the fog has it all; with none, every tree is.
    trees.reach = g_args.no_fog ? 0.0f : g_args.day ? TREES_REACH_DAY : TREES_REACH_NIGHT;
    trees_build(&trees, &kit, g_scene, (unsigned int)g_args.seed);
    load_seam(engine, "trees");
    if (!g_args.no_woods)
        woods_build(&kit, engine, g_scene, &trees, &breaches, (unsigned int)g_args.seed);
    load_seam(engine, "woods");
    // The lake (spec 13.41): its water, and in a kit of its own the ring, the dock, the boats,
    // the reeds and the drowned trees' bodies; then the cabin on its bank, in a kit of its own
    // too. All of it is drawn, and the water on, only from within the fog's reach of it.
    const float lake_reach = trees.reach > 0.0f ? trees.reach + 15.0f : 0.0f;
    Kit lake;
    kit_init_beside(&lake, &kit, GLM_VEC3_ZERO);
    lake.draw_distance = lake_reach;
    lake_build(&g_lake, &lake, g_scene, &trees, (unsigned int)g_args.seed, lake_reach);
    trees_release(&trees);
    Kit cabin;
    kit_init_beside(&cabin, &kit, GLM_VEC3_ZERO);
    cabin.draw_distance = lake_reach;
    cabin_build(&cabin, (unsigned int)g_args.seed);
    load_seam(engine, "lake-cabin");
    grounds_build(&kit, g_scene, (unsigned int)g_args.seed, !g_args.day, &g_failing[FAILING_DRIVE]);
    load_seam(engine, "grounds");

    // The Gothic house as the mansion at the end of the street (spec 13.25): the same plan, built
    // in a kit of its own that stands it where layout.h says.
    Kit mansion;
    const vec3 mansion_origin = {MANSION_X, MANSION_Y, MANSION_Z};
    kit_init_beside(&mansion, &kit, mansion_origin);
    house_build(&mansion);
    interior_build(&mansion);
    load_seam(engine, "house-interior");
    hearth_build(&mansion);
    study_build(&mansion, g_scene, (unsigned int)g_args.seed);
    mansion_front_build(&mansion);
    load_seam(engine, "hearth-study-front");

    Kit* const kits[] = {&kit, &mansion, &ground, &lake, &cabin};
    const char* const kit_names[] = {"world", "mansion", "ground", "lake", "cabin"};
    for (int i = 0; i < KIT_COUNT(kits); i++) {
        kit_finish(kits[i], kit_names[i]);
        load_seam(engine, kit_names[i]);
    }
    kit_free_unused(kits, KIT_COUNT(kits));
    for (int i = 0; i < KIT_COUNT(kits); i++)
        printf("silent: %s: %d colliders, %d vertices in %d meshes and %d shadow cells, %d drip "
               "lines, %d candles\n",
               kit_names[i], kits[i]->collider_count, kits[i]->vertex_count, kits[i]->mesh_count,
               kits[i]->shadow_cell_count, kits[i]->drip_count, kits[i]->wick_count);
    // The rain takes every drip line in one list.
    RainDripLine drips[RAIN_DRIP_MAX];
    int drip_count = 0;
    for (int i = 0; i < KIT_COUNT(kits); i++)
        for (int d = 0; d < kits[i]->drip_count && drip_count < RAIN_DRIP_MAX; d++)
            drips[drip_count++] = kits[i]->drips[d];
    printf("silent: %d of %d drip lines\n", drip_count, RAIN_DRIP_MAX);
    g_scene->fire = create_fire_system();
    if (!g_args.no_candles) {
        candles_light(g_scene->fire, g_scene, &kit, !g_args.no_candle_shadows);
        candles_light(g_scene->fire, g_scene, &mansion, !g_args.no_candle_shadows);
        candles_light(g_scene->fire, g_scene, &cabin, !g_args.no_candle_shadows);
    }
    // The cabin's hearth, which is no candle and burns however they stand (spec 13.41).
    cabin_light(g_scene->fire, g_scene, !g_args.no_candle_shadows);
    load_seam(engine, "candles");
    g_door_hung[DOOR_HOME] = home_front_door(&g_doors[DOOR_HOME], engine, g_scene, em, physics);
    g_door_hung[DOOR_BATH] = home_bath_door(&g_doors[DOOR_BATH], engine, g_scene, em, physics);
    g_door_hung[DOOR_BEDROOM] =
        home_bedroom_door(&g_doors[DOOR_BEDROOM], engine, g_scene, em, physics);
    g_door_hung[DOOR_BASEMENT] =
        home_basement_door(&g_doors[DOOR_BASEMENT], engine, g_scene, em, physics);
    g_door_hung[DOOR_MANSION] =
        house_front_door(&g_doors[DOOR_MANSION], engine, g_scene, em, physics, mansion_origin);
    g_door_hung[DOOR_CABIN] = cabin_door(&g_doors[DOOR_CABIN], engine, g_scene, em, physics);
    // Left standing open by whoever went out, the fire showing through it down to the dock.
    if (g_door_hung[DOOR_CABIN])
        door_set_open(&g_doors[DOOR_CABIN]);
    load_seam(engine, "doors");
    // The backpack on the bedroom's bed (spec 13.40), and the town map on the fridge (spec 13.43),
    // unless the run starts with them.
    backpack_build(&g_backpack, engine, g_scene);
    fridge_map_build(&g_fridge_map, engine, g_scene);
    if (g_args.backpack)
        backpack_take(&g_backpack);
    if (g_args.map)
        backpack_add(&g_backpack, ITEM_TOWN_MAP);
    if (g_args.key)
        backpack_add(&g_backpack, ITEM_KEY);
    fridge_map_on_door(&g_fridge_map, !backpack_holds(&g_backpack, ITEM_TOWN_MAP));
    // The map's print shows one seed's street, and another seed builds other houses.
    if ((unsigned int)g_args.seed != MAP_ART[MAP_TOWN].seed)
        fprintf(stderr, "silent: the town map shows seed %u's street, not seed %d's\n",
                MAP_ART[MAP_TOWN].seed, g_args.seed);
    hud_start(&g_hud, engine);
    backpack_menu_start(&g_menu, g_hud.ui, g_hud.font, &g_backpack);
    map_screen_start(&g_map_screen, g_hud.ui, engine, g_scene->tex_pool);
    if (g_args.open_backpack != ITEM_NONE) {
        backpack_menu_show(&g_menu);
        backpack_menu_choose(&g_menu, g_args.open_backpack);
    }
    memcpy(g_finds.found, g_args.found, sizeof(g_finds.found));
    if (g_args.open_map)
        map_screen_show(&g_map_screen, MAP_TOWN, g_finds.found);
    load_seam(engine, "backpack-hud");
    // Every static collider stands by now, and the cat's place check casts thousands of rays at
    // them: unoptimised, the broadphase is a chain each ray walks body by body. What comes after --
    // the cat's body, the player's -- the steps keep tidy.
    physics_world_optimize(physics);
    load_seam(engine, "physics-optimize");
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
    load_seam(engine, "cat");

    // Sound: the clock's beat, the tubes' buzz, the fridge and the wind, each
    // heard from where it is. Headless, the system opens no device, so a
    // capture is unchanged by it.
    AudioSystem* audio = create_audio_system(engine->headless);
    if (audio) {
        game_set_audio_system(game, audio);
        // Every sound below decodes on the audio's job thread while the rest loads, and
        // game_run waits for them before the first frame (spec 13.39).
        audio_system_begin_loading(audio);
    }
    const Door* swung[SOUNDS_DOORS] = {[SOUNDS_DOOR_HOME] = hung_door(DOOR_HOME),
                                       [SOUNDS_DOOR_MANSION] = hung_door(DOOR_MANSION),
                                       [SOUNDS_DOOR_BASEMENT] = hung_door(DOOR_BASEMENT),
                                       [SOUNDS_DOOR_CABIN] = hung_door(DOOR_CABIN)};
    load_seam(engine, "audio-device");
    sounds_start(&g_sounds, audio, swung);
    load_seam(engine, "audio-sounds");
    kitchen_start_audio(&g_fridge, audio);
    lake_start_audio(&g_lake, audio);
    cabin_start_audio(audio);
    clock_start(&g_clock, engine, g_scene, audio);
    load_seam(engine, "audio-kitchen-clock");
    basement_start(&g_basement, engine, g_scene, audio, (unsigned int)g_args.seed);
    lights_start_audio(&g_lights, audio);
    load_seam(engine, "audio-basement-lights");
    tv_start_audio(&g_tv, audio);
    cat_voice_start(&g_voice, &g_cat, audio, g_args.cat_say);
    load_seam(engine, "audio-tv-cat");

    // Before the sky: its reflections are baked through the fog set here.
    build_post(engine, !g_args.day, !g_args.no_grade);
    load_seam(engine, "post");
    build_sky(engine);
    load_seam(engine, "sky");

    if (!g_args.no_wind) {
        Wind* wind = create_wind("street");
        if (wind) {
            glm_vec3_copy((vec3){0.0f, 0.0f, 1.0f}, wind->direction);
            wind->air_speed = WIND_AIR_SPEED;
            wind->gust_frequency = WIND_GUST_FREQUENCY;
            wind->gust_amount = WIND_GUST_AMOUNT;
            // Every tree in the woods is a copy of a handful, and without this they sway as one.
            wind->phase_variation = 1.0f;
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
            rain_set_drip_lines(g_scene->rain, drips, drip_count);
            rain_settle(g_scene->rain);
        }
    }
    rain_bed_start(&g_rain_bed, audio, g_scene->rain);
    load_seam(engine, "wind-rain");

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
        if (g_args.tile_stores >= 0)
            ss->tile_store_cells = g_args.tile_stores;
    }
    // The play budget is the engine's, or the one the run pinned; capture_budget gives the
    // captures more while nothing is up to wait on them.
    if (g_args.capture_budget_ms >= 0.0f)
        engine->capture_budget_ms = g_args.capture_budget_ms;
    g_play_capture_ms = engine->capture_budget_ms;

    // In the kitchen's back corner looking across it to the hall door, or wherever --player-at
    // put the player, on whatever stands under them there -- the ground, a floor, the dock --
    // looking along its yaw.
    vec3 feet, look;
    glm_vec3_copy((float*)SPAWN_FEET, feet);
    glm_vec3_copy((float*)SPAWN_LOOK_AT, look);
    if (g_args.player_at_set) {
        const float x = g_args.player_at[0], z = g_args.player_at[1];
        // From two metres over the ground or the lake's water, under any roof over them.
        vec3 from = {x, fmaxf(land_height(x, z), LAKE_Y) + 2.0f, z};
        RaycastHit hit;
        const bool stood = physics_world_raycast_filtered(physics, from, (vec3){0.0f, -1.0f, 0.0f},
                                                          30.0f, 1u << OBJ_LAYER_STATIC, &hit);
        glm_vec3_copy((vec3){x, (stood ? hit.position[1] : land_height(x, z)) + 0.05f, z}, feet);
        const float yaw = glm_rad(g_args.player_at[2]);
        glm_vec3_copy((vec3){x + sinf(yaw), feet[1] + PLAYER_EYE_HEIGHT, z + cosf(yaw)}, look);
    }
    const float eye_y = feet[1] + PLAYER_EYE_HEIGHT;
    // Its field of view is the settings' own, which they put on it once they are applied.
    CameraDesc cam = {.position = {feet[0], eye_y, feet[2]},
                      .look_at = {look[0], look[1], look[2]},
                      .fov = glm_rad(SETTINGS_FOV),
                      .near = 0.05f,
                      .far = 250.0f};
    engine_set_camera(engine, create_camera(&cam));

    scene_set_capture_rest(g_scene, rest_lights, NULL);
    // The haunting takes hold of the house now, and begins only when it is played.
    haunt_start(&g_haunt,
                &(HauntHouse){.engine = engine,
                              .scene = g_scene,
                              .audio = audio,
                              .clock = &g_clock,
                              .tv = &g_tv,
                              .fridge = &g_fridge,
                              .lights = &g_lights,
                              .basement = &g_basement,
                              .cellar = g_door_hung[DOOR_BASEMENT] ? &g_doors[DOOR_BASEMENT] : NULL,
                              .bath = g_door_hung[DOOR_BATH] ? &g_doors[DOOR_BATH] : NULL,
                              .filament = kit.materials[MAT_FILAMENT],
                              .shade = kit.materials[MAT_LAMPSHADE],
                              .bulbs = kit.materials[MAT_BULB],
                              .panes = kit.materials[MAT_WINDOW_GLASS],
                              .lamp_heads = kit.materials[MAT_LAMP_GLOW]},
                (unsigned int)g_args.seed * 2654435761u + 13u);
    g_haunt.trace = g_args.trace_haunt;
    // The house's story (spec 13.53): its front door locked, the key in the basement.
    story_start(
        &g_story,
        &(StoryHouse){.engine = engine,
                      .scene = g_scene,
                      .audio = audio,
                      .hud = &g_hud,
                      .backpack = &g_backpack,
                      .haunt = &g_haunt,
                      .front = g_door_hung[DOOR_HOME] ? &g_doors[DOOR_HOME] : NULL,
                      .cellar = g_door_hung[DOOR_BASEMENT] ? &g_doors[DOOR_BASEMENT] : NULL});
    g_story.trace = g_args.trace_haunt;
    story_key_on_bench(&g_story, !backpack_holds(&g_backpack, ITEM_KEY));

    const vec3 dir = {look[0] - feet[0], look[1] - eye_y, look[2] - feet[2]};
    player_init(&g_player, game, physics, em, feet, atan2f(dir[0], dir[2]),
                atan2f(dir[1], sqrtf(dir[0] * dir[0] + dir[2] * dir[2])));
    g_player.trace = g_args.trace_gait;
    g_player.interpolate = !g_args.no_interp;
    g_player.use_gait = !g_args.no_gait;
    g_player.gait->bob_scale = g_args.bob;
    g_player.crouch_toggled = g_args.crouch;
    g_player.gait->climb_up *= g_args.climb;
    g_player.gait->climb_down *= g_args.climb;
    footsteps_start(&g_footsteps, g_args.no_footsteps ? NULL : game->audio, physics, &g_surfaces,
                    (uint32_t)g_args.seed * 2654435761u + 1u);
    g_footsteps.trace = g_args.trace_gait;
    load_seam(engine, "player");
    // Sent somewhere or holding a clip from the command line, the cat has no mind of its own.
    if (g_cat.entity && !g_args.cat_go[0] && !g_args.cat_clip[0])
        cat_mind_create(&g_mind, &g_cat, game, &g_player, g_args.cat_seed, g_args.cat_blind,
                        g_args.cat_doing);

    // What a save keeps (spec 13.49), and a game started from one.
    SaveWorld world = {.game = game,
                       .player = &g_player,
                       .backpack = &g_backpack,
                       .fridge_map = &g_fridge_map,
                       .finds = &g_finds,
                       .map_screen = &g_map_screen,
                       .lights = &g_lights,
                       .basement = &g_basement,
                       .cat = &g_cat,
                       .mind = &g_mind};
    for (int i = 0; i < DOORS; i++)
        if (g_door_hung[i])
            world.doors[world.door_count++] = &g_doors[i];
    save_game_start(&world);
    // What the player chose (spec 13.50), on the audio, the window, the camera and the player's
    // rig, which is installed until the title's is.
    settings_menu_start(&g_settings, g_hud.ui, engine, game->audio);
    settings_menu_apply(&g_settings);
    // The title, over the street, unless the run starts somewhere of its own -- headless, or
    // elsewhere on the command line -- and then it saves nowhere unless --slot says where. A run
    // whose UI could not be made has no title to wait at. One with a title can go back to it, the
    // world put back to how it stands now.
    const bool titled =
        (!engine->headless || g_args.title) && !starts_elsewhere(&g_args) && g_hud.ui;
    if (titled)
        title_start(&g_title, g_hud.ui, game, &g_settings);
    else
        enter_game(engine, g_args.load, g_args.slot);
    const bool can_return = titled && save_game_keep_start();
    pause_menu_start(&g_pause, g_hud.ui, game, &g_settings, can_return);

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
    load_seam(engine, "mind-exposure");
    g_settle_start = g_load_mark;
}

static void on_update(Game* game, double dt) {
    // The cat first, while the player's body still holds the velocity the last step asked of it,
    // which the player's own update replaces with this step's.
    vec3 feet = {0.0f, 0.0f, 0.0f};
    player_feet(&g_player, feet);
    cat_step(&g_cat, feet, (float)dt);
    // What it senses is decided here, and what it does about it by its brain after this hook.
    cat_mind_sense(&g_mind, g_args.rain_mmh / CAT_HEAVY_RAIN, (float)dt);
    player_update(&g_player, game, dt);
    // Nobody goes into the dark down there without a light, and at the cellar flight's foot the
    // player thinks why they can go no further (spec 13.40).
    if (basement_hold(&g_basement, game->entity_manager, game->physics_world, feet,
                      backpack_holds(&g_backpack, ITEM_FLASHLIGHT)))
        hud_think(&g_hud, "It's too dark. I can't see. I need to find a flashlight.");
    // What goes on the map (spec 13.43): a road out of town that cannot be followed, the cabin by
    // the lake. With the map, a find is marked on it and M is hinted; without, it is remembered
    // and written on once the map is had.
    bool first = false;
    const PlaceId place = finds_arrive(&g_finds, feet, &first);
    if (place != PLACE_NONE) {
        const bool mapped = backpack_holds(&g_backpack, ITEM_TOWN_MAP);
        const char* thought = !first   ? PLACES[place].again
                              : mapped ? PLACES[place].found_mapped
                                       : PLACES[place].found;
        if (thought)
            hud_think(&g_hud, thought);
        if (first && mapped)
            hud_hint(&g_hud, MAP_HINT, HINT_SECONDS);
        if (first)
            save_game_due();
    }
    for (int i = 0; i < DOORS; i++)
        if (g_door_hung[i])
            door_update(&g_doors[i], (float)dt);
    // After the doors: the haunting moves the cellar door's leaf in its frame, which the step's end
    // carries to its body and node with the rest. Its door beats wait for where the player looks.
    vec3 forward = {0.0f, 0.0f, -1.0f};
    if (game->engine->camera_rig)
        camera_rig_direction(game->engine->camera_rig, forward);
    haunt_step(&g_haunt, game->time, feet, forward);
    // The player's try at a door the story keeps shut (spec 13.53).
    story_step(&g_story, game->time);
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

// What the action key would act on: the nearest of what is in reach, a door to swing or a thing to
// take, `prompt` naming the taking.
typedef struct Reach {
    float d;
    Door* door;
    void (*take)(void);
    const char* prompt;
} Reach;

// `at` in the running for `reach`, as a door or a thing to take.
static void reach_offer(Reach* reach, const vec3 at, Door* door, void (*take)(void),
                        const char* prompt) {
    const float d = player_reach_distance(&g_player, at);
    if (d < reach->d)
        *reach = (Reach){d, door, take, prompt};
}

// The bag off the bed (spec 13.40): the player has it and what is in it, and says so.
static void take_backpack(void) {
    backpack_take(&g_backpack);
    hud_think(&g_hud, "My backpack. There's a flashlight in it.");
    hud_hint(&g_hud, "Tab   Backpack", HINT_SECONDS);
    save_game_due();
}

// The map off the fridge (spec 13.43): into the backpack, once there is a backpack to put it in.
static void take_map(void) {
    if (!backpack_add(&g_backpack, ITEM_TOWN_MAP)) {
        hud_think(
            &g_hud,
            "I don't have anywhere to put this. Maybe I should find a backpack or something.");
        return;
    }
    fridge_map_on_door(&g_fridge_map, false);
    hud_think(&g_hud, finds_any(&g_finds) ? "A map of the town. I'll mark what I've found on it."
                                          : "A map of the town. That'll come in handy.");
    hud_hint(&g_hud, MAP_HINT, HINT_SECONDS);
    save_game_due();
}

// The front door's key off the basement's workbench (spec 13.53), into the backpack.
static void take_key(void) {
    if (!backpack_add(&g_backpack, ITEM_KEY)) {
        hud_think(&g_hud, "I don't have anywhere to put this.");
        return;
    }
    story_key_on_bench(&g_story, false);
    hud_think(&g_hud, "A key. It might fit the front door.");
    save_game_due();
}

// A foot the gait let fall this frame, heard where the drawn `feet` came down (spec 13.47); or,
// under --footstep-say, every take in turn.
static void footfall(const Game* game, const vec3 feet) {
    if (g_args.footstep_say) {
        footsteps_say(&g_footsteps, game->time, feet);
        return;
    }
    if (g_player.gait->footfall.kind == GAIT_FALL_NONE)
        return;
    // The rain bed knows whether the head is under open sky, a frame late.
    const Rain* rain = g_scene->rain;
    const float wetness = rain && g_rain_bed.open > 0.5f ? rain->wetness : 0.0f;
    footsteps_fall(&g_footsteps, &g_player.gait->footfall, feet, g_player.rig->yaw, wetness,
                   player_crouched(&g_player));
}

static void on_pre_render(Game* game, double alpha) {
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

    // The flashlight is in the backpack (spec 13.40): until the player has it, F finds nothing.
    if (input_action_pressed(&game->input, "flashlight")) {
        if (backpack_holds(&g_backpack, ITEM_FLASHLIGHT))
            lights_toggle_flashlight(&g_lights);
        else
            hud_think(&g_hud, "I don't have a flashlight.");
    }
    // Where the view is: the installed rig's, the title's over the street while it is up and the
    // player's after. Its pose is this frame's, before the engine applies it.
    vec3 eye = {0.0f, 0.0f, 0.0f}, forward = {0.0f, 0.0f, -1.0f};
    if (engine->camera_rig) {
        glm_vec3_copy(engine->camera_rig->pose.eye, eye);
        camera_rig_direction(engine->camera_rig, forward);
    }
    // The lake's water while the eye is near it, and a wake where the player wades (spec 13.41).
    vec3 feet = {0.0f, 0.0f, 0.0f};
    player_view_feet(&g_player, feet);
    lake_update(&g_lake, eye, feet);
    footfall(game, feet);

    // The nearest thing the player is looking at in reach -- a door, the backpack on the bed or the
    // map on the fridge -- says what the action key would do to it, and the key does it. Nothing is
    // in reach of a player who is not yet playing, or behind a screen that holds the game's keys.
    Reach reach = {.d = FLT_MAX};
    if (g_run == RUN_PLAYING && !ui_captures_input(g_hud.ui)) {
        for (int i = 0; i < DOORS; i++)
            if (g_door_hung[i])
                reach_offer(&reach, g_doors[i].entity->position, &g_doors[i], NULL, NULL);
        if (!g_backpack.taken)
            reach_offer(&reach, g_backpack.at, NULL, take_backpack, "E   Take the backpack");
        if (!backpack_holds(&g_backpack, ITEM_TOWN_MAP))
            reach_offer(&reach, g_fridge_map.at, NULL, take_map, "E   Take the map");
        if (g_story.key && !backpack_holds(&g_backpack, ITEM_KEY))
            reach_offer(&reach, g_story.key_at, NULL, take_key, "E   Take the key");
    }
    if (input_action_pressed(&game->input, "interact")) {
        if (reach.take)
            reach.take();
        else if (reach.door && !story_door(&g_story, reach.door, game->time))
            door_toggle(reach.door);
    }
    // What is drawn over the game eases by the frame, as the head turns, and what only looks
    // goes on while the game is held.
    const float dt = (float)look_dt(game);
    const double looks_at = look_time(game);
    hud_prompt(&g_hud, reach.take ? reach.prompt
                       : reach.door
                           ? (door_will_open(reach.door) ? "E   Open door" : "E   Close door")
                           : NULL);
    hud_update(&g_hud, dt, (float)engine->win_height);
    backpack_menu_update(&g_menu, dt);
    map_screen_update(&g_map_screen, dt, feet, forward);
    sounds_update(&g_sounds);
    // The beam trails the head by the frame, as the head turns.
    lights_update(&g_lights, g_scene, looks_at, dt, eye, forward);
    for (int i = 0; i < FAILING_LAMPS; i++)
        failing_lamp_update(&g_failing[i], looks_at);
    tv_update(&g_tv, looks_at);
    cat_mind_frame(&g_mind, game->time);
    cat_debug_draw(&g_cat, &g_mind, engine);
    cat_update(&g_cat, game, g_scene, &g_lights, eye, (float)game->sim_clock.delta);
    cat_voice_update(&g_voice, eye, cat_mind_at_ease(&g_mind), (float)game->sim_clock.delta);
    clock_update(&g_clock, game->time);
    basement_update(&g_basement, hung_door(DOOR_BASEMENT), game->time);
    // The rain on the roof, heard through the floor from the basement.
    rain_bed_update(&g_rain_bed, g_scene->rain, g_scene->shadow_system, eye, g_sounds.past_walls,
                    (float)game->sim_clock.delta);
    dump_audio(game);

    // The volume goes in on the third frame, not at load. The tubes' panels are
    // derived during the first frame's draw and only cast from the next, and a
    // volume's FIRST sweep is the only one taken at full weight -- a re-arm
    // blends into what is already there -- so it has to see the lit room. The
    // reflection probes go in with it, since they are captured once it has
    // converged and a set installed with no volume would be captured unlit.
    if (engine->total_frames == 2 && !g_scene->gi && !g_args.no_gi) {
        build_home_gi();
        build_gi((vec3){MANSION_X, MANSION_Y, MANSION_Z});
        build_cabin_gi();
        build_probes();
        if (g_scene->gi) {
            g_scene->gi->cull_pixels = g_args.gi_cull_pixels;
            g_scene->gi->timing = g_args.capture_timing;
        }
        // Cooked (spec 13.42): a launch after the first loads the houses' light from disk instead
        // of sweeping it, unless something it sees has changed. CETRA_NO_COOK=1 sweeps.
        g_scene->cook_lighting = true;
    }
    // Black until the opening sweep of the volume the eye is in has landed, and the reflection
    // probes round the eye with it, then up -- not every volume's: the mansion's sweeps only once
    // the drive brings its candles near. The sweep is spread over frames (spec 13.32), so without
    // this the room shows unlit by its own bounce light while it comes in, and its reflections
    // after it. A volume that could not be built lets the view up rather than holding it dark
    // forever. The eye is the title's while it is up and the player's once a game is chosen.
    AABB at;
    aabb_empty(&at);
    aabb_add_point(&at, eye);
    const bool lit = engine->total_frames > 2 && scene_lighting_ready_in(g_scene, &at);
    const bool screen = engine_loading_screen_shown(engine);
    switch (g_run) {
        case RUN_LOADING:
            // Lit, the game is loaded and the screen is let go. It switches off once its ident has
            // played (spec 13.34), and what comes up waits for that: the title when the run has
            // one, the game when it does not. A return to the title loads here again, black.
            if (lit)
                engine_hide_loading_screen(engine);
            if (lit && !screen) {
                if (title_show(&g_title)) {
                    g_run = RUN_TITLE;
                    title_open_screen(&g_title, g_args.title_screen);
                    g_args.title_screen = NULL;
                } else {
                    start_play();
                }
            }
            break;
        case RUN_TITLE:
            if (g_title.choice != TITLE_WAITING)
                g_run = RUN_LEAVING;
            break;
        case RUN_LEAVING:
            if (g_fade > 0.0f)
                break;
            if (enter_game(engine, g_title.choice == TITLE_LOAD ? g_title.chosen.name : NULL,
                           g_title.chosen.name)) {
                g_run = RUN_ENTERING;
            } else {
                title_refused(&g_title);
                g_run = RUN_TITLE;
            }
            break;
        case RUN_ENTERING:
            if (lit)
                start_play();
            break;
        case RUN_PLAYING:
            // Quit to Title (spec 13.50): the game kept as it stands, then saved no more, and black
            // while the world is put back.
            if (pause_menu_take_to_title(&g_pause)) {
                save_game_quit();
                save_game_begin(NULL);
                haunt_end(&g_haunt);
                g_run = RUN_RETURNING;
            } else if (save_game_frame(feet, game->frame_clock.delta)) {
                hud_saved(&g_hud);
            }
            break;
        case RUN_RETURNING:
            // At black, the world back to the start kept at load and nothing on the screen, the
            // title's view in place of the player's, and the title up once the lighting round it is
            // in. A start that cannot be read would leave the played world under the title, so the
            // game, already saved, ends instead.
            if (g_fade > 0.0f)
                break;
            if (!save_game_load_start()) {
                game_quit(game);
                break;
            }
            hud_clear(&g_hud);
            title_view(&g_title, engine);
            g_run = RUN_LOADING;
            break;
    }
    // The haunting (spec 13.52): begun as the game is first played when the command line asks,
    // and by hand. It plays out on the sim's clock, so it stands still with the game.
    if (g_run == RUN_PLAYING) {
        if (g_args.haunt) {
            g_args.haunt = false;
            haunt_begin(&g_haunt, game->time, g_args.haunt_at);
        } else if (input_action_pressed(&game->input, "haunt")) {
            if (haunt_active(&g_haunt))
                haunt_end(&g_haunt);
            else
                haunt_begin(&g_haunt, game->time, 0.0);
        }
    }
    haunt_frame(&g_haunt, game->time);
    // A step is what puts each entity's node where the entity is, and a held game runs none: so
    // while it is held -- loading, at the title, under the pause menu, over a load -- the doors and
    // the cat are put where they stand here, before the frame and its captures draw them.
    if (game->paused)
        sync_entity_transforms(game->entity_manager);
    capture_budget(engine);
    // Up on the title and in the game, black between: the fade rides the grade's gain, after the
    // tonemap, so the exposure and the day's meter never see it. It eases by the frame, so a game
    // paused while it comes up still comes up, and each frame's step is capped because a frame of
    // captures can be long.
    const bool up = g_run == RUN_TITLE || g_run == RUN_PLAYING;
    const float step = fminf((float)frame_dt(game), 1.0f / 30.0f);
    g_fade = up ? fminf(g_fade + step / FADE_IN_SECONDS, 1.0f)
                : fmaxf(g_fade - step / FADE_OUT_SECONDS, 0.0f);
    if (engine->postfx)
        glm_vec3_fill(engine->postfx->grade_gain, glm_smoothstep(0.0f, 1.0f, g_fade));
    if (g_args.startup_ms)
        trace_settling(engine, lit, up);
}

/*
 * The screens over the game (specs 13.40, 13.43 and 13.50), what is on top deciding what a key
 * does:
 *
 *   on top            Tab                     M                         Escape / pad B
 *   the game          the bag, if had         the map, if had           the pause menu (and Start)
 *   the bag           closes it               the map over the bag      closes it
 *   the map           closes it, and the bag  closes it                 closes it
 *                     comes up if it was not
 *                     already under it
 *   any other screen  --                      --                        closes it, but for the
 *                                                                       title's own first screen
 *
 * A close is the UI's own back, which pops what is on top and is raised only while a screen is, so
 * the HUD under them is never taken down. The state is read from what is on top at the frame's
 * start, so the key that opened a screen cannot close it in the same frame, and the Start that
 * opens the pause menu is not also an accept on Resume. Here, before the fixed steps, so nobody
 * moves under the loading screen or a screen, and the frame that opens one walks no step. The
 * game's input is held through the frame that shuts one too, so the key that shut it reaches
 * nothing else, and the cursor the input freed comes back after.
 */
static void on_frame_input(Game* game) {
    const Engine* engine = game->engine;
    const bool playing = g_run == RUN_PLAYING;
    const bool modal_up = ui_captures_input(g_hud.ui);
    const bool tab = playing && input_action_pressed(&game->input, "backpack");
    const bool m = playing && input_action_pressed(&game->input, "map");
    const bool esc = input_action_pressed(&game->input, "menu_back");
    const bool pause = playing && input_action_pressed(&game->input, "pause_menu");
    const bool have_map = backpack_holds(&g_backpack, ITEM_TOWN_MAP);
    bool back = false, bag_after = false;
    if (!modal_up) {
        if (pause)
            pause_menu_show(&g_pause);
        if (tab && !pause) {
            if (g_backpack.taken)
                backpack_menu_show(&g_menu);
            else
                hud_think(&g_hud, "I'm not carrying anything.");
        }
        if (m && !pause) {
            if (have_map)
                map_screen_show(&g_map_screen, MAP_TOWN, g_finds.found);
            else
                hud_think(&g_hud, "I don't have a map.");
        }
    } else if (map_screen_open(&g_map_screen)) {
        back = tab || m || esc;
        bag_after = tab;
    } else if (backpack_menu_open(&g_menu)) {
        back = tab || esc;
        if (m && have_map)
            map_screen_show(&g_map_screen, MAP_TOWN, g_finds.found);
    } else {
        back = esc && !title_on_main(&g_title);
    }
    double mx = 0.0, my = 0.0;
    input_mouse_pos(&game->input, &mx, &my);
    const UIInput in = {
        .pointer_x = (float)mx,
        .pointer_y = (float)my,
        .pointer_down = input_mouse_down(&game->input, GLFW_MOUSE_BUTTON_LEFT),
        .pointer_pressed = input_mouse_pressed(&game->input, GLFW_MOUSE_BUTTON_LEFT),
        .pointer_released = input_mouse_released(&game->input, GLFW_MOUSE_BUTTON_LEFT),
        .nav_up = input_action_pressed(&game->input, "menu_up"),
        .nav_down = input_action_pressed(&game->input, "menu_down"),
        .nav_left = input_action_pressed(&game->input, "menu_left"),
        .nav_right = input_action_pressed(&game->input, "menu_right"),
        .accept = !pause && input_action_pressed(&game->input, "menu_accept"),
        .back = back,
    };
    // Laid out to the window as it is now, before the UI's pass hit-tests it.
    const float w = (float)engine->win_width, h = (float)engine->win_height;
    backpack_menu_layout(&g_menu, w, h, w > 0.0f ? (float)engine->fb_width / w : 1.0f);
    map_screen_layout(&g_map_screen, w, h);
    ui_update(g_hud.ui, &in, w, h);
    if (input_action_pressed(&game->input, "menu_delete"))
        title_ask_delete(&g_title);
    settings_menu_frame(&g_settings);
    // Nothing to do when the map was over the bag, which its pop has just put back on top.
    if (bag_after)
        backpack_menu_show(&g_menu);
    backpack_menu_input(&g_menu, &in);
    double sx = 0.0, sy = 0.0;
    input_scroll(&game->input, &sx, &sy);
    const MapControls controls = {
        .pan = {input_action_value(&game->input, "map_pan_x"),
                input_action_value(&game->input, "map_pan_y")},
        .zoom = input_action_value(&game->input, "map_zoom"),
        .wheel = (float)sy,
    };
    map_screen_input(&g_map_screen, &in, &controls, (float)engine->render_delta);
    // Nobody moves before the game is played -- under the loading screen, at the title, or while
    // the chosen game comes up out of black -- nor while the pause menu is up, under which the
    // world stands still as well.
    const bool pause_up = pause_menu_up(&g_pause);
    hold_game(game, !playing ? HOLD_HELD : pause_up ? HOLD_FROZEN : HOLD_PLAYED);
    input_set_suppressed(&game->input, !playing || modal_up || ui_captures_input(g_hud.ui));
}

// Before the engine goes: the HUD draws through its overlay hook, and the backpack's models -- and
// the bag and the map once taken -- are on no graph the scene would free.
static void on_shutdown(Game* game) {
    if (game && game->engine)
        profiler_report(game->engine->profiler);
    if (g_args.tiles_probe && g_scene)
        shadow_tiles_probe(g_scene->shadow_system, g_scene);
    // While every entity is still alive.
    if (g_run == RUN_PLAYING)
        save_game_quit();
    save_game_free();
    // Settings changed and the window shut over them, with no screen gone to write them.
    settings_menu_save(&g_settings);
    title_free(&g_title);
    backpack_menu_free(&g_menu);
    map_screen_free(&g_map_screen);
    hud_free(&g_hud);
    backpack_free(&g_backpack);
    fridge_map_free(&g_fridge_map);
    story_free(&g_story);
    cat_free(&g_cat);
    player_free(&g_player);
    kit_surfaces_release(&g_surfaces);
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
    printf("      --crt               Watched on a consumer CRT television\n");
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
    printf("      --fog-history-miss-samples N  Samples a fog cell entering the view takes,\n"
           "                          1..%d (1 = its one sample, which bands the fog's edge\n"
           "                          on a turn)\n",
           POSTFX_FOG_MISS_SAMPLES_MAX);
    printf("      --render-scale F    Render at F of the window and upscale (0.5-1, default\n");
    printf("                          %.1f): the softer frame of the consoles it imitates\n",
           (double)DEFAULT_RENDER_SCALE);
    printf("      --msaa N            MSAA samples\n");
    printf("      --cam-eye x,y,z     Pin the camera (a framing that can be taken twice)\n");
    printf("      --cam-target x,y,z  What the pinned camera looks at\n");
    printf("      --player-at x,z[,yaw]  Start standing there rather than in the kitchen,\n"
           "                          facing yaw degrees from +z (south)\n");
    printf("      --fov D             Vertical field of view, degrees (default 68)\n");
    printf("      --pad-script PATH   Replay a scripted pad on slot 0 (see input.h)\n");
    printf("      --trace-player      Print the player's position every 30 steps\n");
    printf(
        "      --trace-gait        Print every step's move: feet, rise, run, ground and speed\n");
    printf(
        "      --no-interp         Draw the body where the last step left it, not between two\n");
    printf("      --no-gait           The eye rides the body rigidly, and a climb is not paced\n");
    printf(
        "      --bob X             The head's bob as a share of the gait's (default 1; 0 none)\n");
    printf("      --climb X           The pace up and down a flight as a share of the gait's "
           "(default 1)\n");
    printf("      --no-footsteps      Walk without a sound\n");
    printf("      --footstep-say      Play every footstep take in turn, two a second\n");
    printf("      --crouch            Start crouched, as though C had been pressed\n");
    printf("      --no-flicker        Keep the failing ceiling tube steady\n");
    printf("      --no-static         The living room's television with nothing on it, no snow\n");
    printf("      --haunt             The haunting begins as the game is played (K begins and\n"
           "                          ends it)\n");
    printf("      --haunt-at S        The haunting begun S seconds before the game is played, its\n"
           "                          door beats too, without waiting to be watched for\n");
    printf("      --trace-haunt       Print each of the haunting's beats as it comes\n");
    printf("      --no-loading-screen Load under a black window, without the engine's screen\n");
    printf("      --loading-screen    The engine's loading screen headless too\n");
    printf("      --startup-ms        Print how long each loading step and held frame took\n");
    printf("      --flashlight        Start with the flashlight, and on (F toggles it)\n");
    printf("      --backpack          Start with the backpack, the flashlight in it\n");
    printf("      --map               Start with the town map in the backpack, the fridge bare\n");
    printf("      --key               Start with the front door's key in the backpack, the\n"
           "                          basement's workbench bare\n");
    printf("      --open-map          Start with the map's screen open, over the backpack's\n"
           "                          with --open-backpack; the map had\n");
    printf("      --found LIST        Start with places found, written on the map when it\n"
           "                          next opens: barricade,road-end,cabin or all (home\n"
           "                          is found with the map)\n");
    printf("      --open-backpack ITEM  Start with the backpack's screen open on ITEM\n"
           "                          (flashlight, map, key), and the backpack had\n");
    printf("      --mute              Without sound\n");
    printf("      --audio-dump PATH   Headless: write what the listener hears as a WAV\n");
    printf("      --map-export PATH   Write the town's plan for tools/make_map.py and stop\n");
    printf("      --rain MM           Rain rate in mm/h (default %.0f)\n",
           (double)DEFAULT_RAIN_MMH);
    printf("      --no-rain           A dry night\n");
    printf("      --no-wind           Still air: the rain falls straight\n");
    printf("      --no-fog            Clear air, to see where everything is\n");
    printf("      --no-relief         Puddles stand where the noise puts them, not in the\n"
           "                          ground's own lows\n");
    printf("      --no-candles        The candles stand unlit\n");
    printf("      --no-candle-shadows The candles light through walls\n");
    printf("      --no-gi             No bounce light: no GI volume, and no reflection probes,\n"
           "                          which are captured from its light\n");
    printf("      --capture-budget-ms F  The most of a frame the light captures may take,\n"
           "                          headless too; by default %.0f ms behind the black and\n"
           "                          the engine's after it, and no limit headless\n",
           (double)LOADING_CAPTURE_MS);
    printf("      --capture-timing    Print what each GI volume's sweep cost, part by part, as\n"
           "                          it converges; slows the sweep\n");
    printf("      --gi-cull-pixels F  A GI probe leaves out what spans fewer pixels than F\n"
           "                          across its %d-pixel faces; 0, the default, takes all\n",
           GI_CAPTURE_FACE);
    printf("      --gi-cell M         The home's GI cell, metres (default %.2f)\n",
           (double)HOME_GI_CELL);
    printf("      --profiler          Per-pass timing and submission counts, at exit\n");
    printf("      --tile-views N      Shade every cached light from N views over its body\n"
           "                          rather than 8; 1 is its centre alone\n");
    printf("      --tile-stores N     Store cells for faces drawn over a copy of their still\n"
           "                          casters (default and at most %d); 0 draws every such\n"
           "                          face whole\n",
           SHADOW_TILE_STORE_CELLS);
    printf("      --tiles-probe       The cached shadow tiles and each light's block, at exit\n");
    printf("      --no-woods          Without the woods behind the yards\n");
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
    printf("  A run starts at the title over the street, in the window, unless it starts from a\n"
           "  save, a new game or any state above -- the player, the bag, the map, the finds,\n"
           "  the crouch, the cat's place or a pinned camera -- and then it saves nowhere unless\n"
           "  --slot says where:\n");
    printf("      --load SLOT         Start from the game saved in SLOT (silent-1 .. silent-%d)\n",
           SAVE_GAME_SLOTS);
    printf("      --new-game          Start a new game without the title\n");
    printf("      --slot SLOT         Save this run into SLOT as it goes\n");
    printf("      --no-autosave       A game from the title that never saves itself\n");
    printf("      --title             The title headless too\n");
    printf("      --title-screen S    The title with S over it: games, or delete (the newest)\n");
    printf("      --pause-screen S    The pause menu up once the game is played: menu, or\n"
           "                          settings over it\n");
    printf("  In the window: click to capture the mouse; Escape or pad Start pauses and\n");
    printf("  frees it. WASD walks, Shift hurries, C crouches or stands and Ctrl\n");
    printf("  crouches while held,\n");
    printf("  the arrows or the mouse look, E opens and shuts\n");
    printf("  a door you are facing or takes what is in reach, F the flashlight once\n");
    printf("  you have it, Tab opens and shuts the backpack, G shows the GUI and frees\n");
    printf("  the mouse for it while it is open, K begins and ends the haunting.\n");
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
    a->tile_stores = -1;
    a->capture_budget_ms = -1.0f;
    a->width = DEFAULT_WIDTH;
    a->height = DEFAULT_HEIGHT;
    a->seed = 7;
    a->render_scale = DEFAULT_RENDER_SCALE;
    a->rain_mmh = DEFAULT_RAIN_MMH;
    a->bob = 1.0f;
    a->climb = 1.0f;
    parse_hex("262424", a->cat_fur);
    parse_hex("E8B923", a->cat_eyes);
    a->cat_clip_seconds = -1.0f;
    a->cat_seed = 1;
    a->open_backpack = ITEM_NONE;
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
        } else if (!strcmp(s, "--crt")) {
            a->crt = true;
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
        } else if (!strcmp(s, "--fog-history-miss-samples") && has_next) {
            a->fog_miss_samples = atoi(argv[++i]);
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
        } else if (!strcmp(s, "--player-at") && has_next) {
            a->player_at[2] = 0.0f;
            a->player_at_set = sscanf(argv[++i], "%f,%f,%f", &a->player_at[0], &a->player_at[1],
                                      &a->player_at[2]) >= 2;
        } else if (!strcmp(s, "--fov") && has_next) {
            a->fov_deg = (float)atof(argv[++i]);
        } else if (!strcmp(s, "--pad-script") && has_next) {
            a->pad_script = argv[++i];
        } else if (!strcmp(s, "--trace-player")) {
            a->trace_player = true;
        } else if (!strcmp(s, "--trace-gait")) {
            a->trace_gait = true;
        } else if (!strcmp(s, "--no-interp")) {
            a->no_interp = true;
        } else if (!strcmp(s, "--no-gait")) {
            a->no_gait = true;
        } else if (!strcmp(s, "--bob") && has_next) {
            a->bob = fmaxf(0.0f, (float)atof(argv[++i]));
        } else if (!strcmp(s, "--climb") && has_next) {
            a->climb = fmaxf(0.01f, (float)atof(argv[++i]));
        } else if (!strcmp(s, "--no-footsteps")) {
            a->no_footsteps = true;
        } else if (!strcmp(s, "--footstep-say")) {
            a->footstep_say = true;
        } else if (!strcmp(s, "--crouch")) {
            a->crouch = true;
        } else if (!strcmp(s, "--no-flicker")) {
            a->no_flicker = true;
        } else if (!strcmp(s, "--no-static")) {
            a->no_static = true;
        } else if (!strcmp(s, "--haunt")) {
            a->haunt = true;
        } else if (!strcmp(s, "--haunt-at") && has_next) {
            a->haunt = true;
            a->haunt_at = fmaxf(0.0f, (float)atof(argv[++i]));
        } else if (!strcmp(s, "--trace-haunt")) {
            a->trace_haunt = true;
        } else if (!strcmp(s, "--no-loading-screen")) {
            a->no_loading_screen = true;
        } else if (!strcmp(s, "--loading-screen")) {
            a->loading_screen = true;
        } else if (!strcmp(s, "--startup-ms")) {
            a->startup_ms = true;
        } else if (!strcmp(s, "--flashlight")) {
            a->flashlight = true;
            a->backpack = true;
        } else if (!strcmp(s, "--backpack")) {
            a->backpack = true;
        } else if (!strcmp(s, "--map")) {
            a->map = true;
        } else if (!strcmp(s, "--key")) {
            a->key = true;
        } else if (!strcmp(s, "--open-map")) {
            a->open_map = true;
        } else if (!strcmp(s, "--found") && has_next) {
            char list[128];
            snprintf(list, sizeof(list), "%s", argv[++i]);
            for (const char* id = strtok(list, ","); id; id = strtok(NULL, ",")) {
                const PlaceId p = place_by_id(id);
                if (!strcmp(id, "all"))
                    for (int k = 0; k < PLACE_COUNT; k++)
                        a->found[k] = true;
                else if (p != PLACE_NONE)
                    a->found[p] = true;
                else
                    fprintf(stderr, "silent: --found names no place '%s'\n", id);
            }
        } else if (!strcmp(s, "--open-backpack") && has_next) {
            a->open_backpack = item_by_id(argv[++i]);
            a->backpack = true;
            if (a->open_backpack == ITEM_NONE)
                fprintf(stderr, "silent: --open-backpack names no item; leaving it shut\n");
        } else if (!strcmp(s, "--mute")) {
            a->mute = true;
        } else if (!strcmp(s, "--audio-dump") && has_next) {
            a->audio_dump = argv[++i];
        } else if (!strcmp(s, "--map-export") && has_next) {
            a->map_export = argv[++i];
        } else if (!strcmp(s, "--rain") && has_next) {
            a->rain_mmh = fmaxf(0.0f, (float)atof(argv[++i]));
        } else if (!strcmp(s, "--no-rain")) {
            a->rain_mmh = 0.0f;
        } else if (!strcmp(s, "--no-wind")) {
            a->no_wind = true;
        } else if (!strcmp(s, "--no-fog")) {
            a->no_fog = true;
        } else if (!strcmp(s, "--no-relief")) {
            a->no_relief = true;
        } else if (!strcmp(s, "--no-candles")) {
            a->no_candles = true;
        } else if (!strcmp(s, "--no-candle-shadows")) {
            a->no_candle_shadows = true;
        } else if (!strcmp(s, "--no-gi")) {
            a->no_gi = true;
        } else if (!strcmp(s, "--capture-budget-ms") && has_next) {
            a->capture_budget_ms = (float)atof(argv[++i]);
        } else if (!strcmp(s, "--capture-timing")) {
            a->capture_timing = true;
        } else if (!strcmp(s, "--gi-cull-pixels") && has_next) {
            a->gi_cull_pixels = (float)atof(argv[++i]);
        } else if (!strcmp(s, "--gi-cell") && has_next) {
            a->gi_cell = (float)atof(argv[++i]);
        } else if (!strcmp(s, "--tile-views") && has_next) {
            a->tile_views = atoi(argv[++i]);
        } else if (!strcmp(s, "--tile-stores") && has_next) {
            a->tile_stores = atoi(argv[++i]);
        } else if (!strcmp(s, "--tiles-probe")) {
            a->tiles_probe = true;
        } else if (!strcmp(s, "--profiler")) {
            a->profiler = true;
        } else if (!strcmp(s, "--no-woods")) {
            a->no_woods = true;
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
        } else if (!strcmp(s, "--slot") && has_next) {
            a->slot = argv[++i];
        } else if (!strcmp(s, "--load") && has_next) {
            a->load = argv[++i];
        } else if (!strcmp(s, "--new-game")) {
            a->new_game = true;
        } else if (!strcmp(s, "--no-autosave")) {
            a->no_autosave = true;
        } else if (!strcmp(s, "--title")) {
            a->title = true;
        } else if (!strcmp(s, "--title-screen") && has_next) {
            a->title_screen = argv[++i];
            a->title = true;
        } else if (!strcmp(s, "--pause-screen") && has_next) {
            a->pause_screen = argv[++i];
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
    // Only what is held can be opened or chosen, the map is had only off the fridge, and it is
    // carried in the backpack.
    a->map |= a->open_map || a->open_backpack == ITEM_TOWN_MAP;
    a->key |= a->open_backpack == ITEM_KEY;
    a->backpack |= a->map || a->key;
    // The plan is written while the world is built, and the run stops there.
    if (a->map_export) {
        a->headless = true;
        a->no_loading_screen = true;
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
    // A window opens as the player chose it (spec 13.50).
    settings_menu_load(&g_settings, g_args.mute, g_args.fov_deg);
    if (!g_args.headless) {
        const GameSettings* chosen = &g_settings.settings;
        config.engine.window_mode = (EngineWindowMode)chosen->window_mode;
        config.engine.monitor = chosen->monitor[0] ? chosen->monitor : NULL;
        config.engine.no_vsync = !chosen->vsync;
    }
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
    game_set_frame_input(game, on_frame_input);
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
