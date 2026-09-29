/*
 * silent: a grimy kitchen at night, its fluorescent tubes buzzing, and a
 * street of houses outside the window that goes on into fog. Walk it in first
 * person, with a flashlight.
 *
 * Everything is procedural -- every mesh is flat-shaded boxes and prisms from
 * kit.c, every texture is baked at startup -- and aimed at the look of a
 * console-era survival horror game: few polygons, low texel density, the dirt
 * painted into the textures rather than modelled, and darkness and fog hiding
 * how little is there.
 */

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
#include "cetra/postfx.h"
#include "cetra/probe.h"
#include "cetra/probe_set.h"
#include "cetra/scene.h"
#include "cetra/shadow.h"
#include "cetra/sky.h"

#include "cetra/game/entity.h"
#include "cetra/game/game.h"
#include "cetra/game/input.h"
#include "cetra/game/physics.h"

#include "house.h"
#include "kitchen.h"
#include "kit.h"
#include "layout.h"
#include "lights.h"
#include "mats.h"
#include "player.h"
#include "street.h"

#define DEFAULT_WIDTH  1600
#define DEFAULT_HEIGHT 900

typedef struct SilentArgs {
    int headless;
    int frames;
    const char* screenshot;
    int screenshot_every;
    int width, height;
    int seed;
    int day;
    int no_taa;
    int no_grade;
    float render_scale;
    int msaa;
    bool cam_eye_set, cam_target_set;
    vec3 cam_eye, cam_target;
    float fov_deg;
    const char* pad_script;
    int trace_player;
    int no_flicker;
    int flashlight;
} SilentArgs;

static SilentArgs g_args;
static Scene* g_scene;
static Player g_player;
static Lights g_lights;

// The spawn: in the kitchen, facing the window.
static const vec3 SPAWN_FEET = {1.5f, FLOOR_Y, 13.2f};
static const float SPAWN_YAW = GLM_PIf; // toward -z, the street

// Keys the app itself reads, beside the player's table.
static const InputAction APP_ACTIONS[] = {
    {"toggle_gui", {INPUT_KEY(GRAVE_ACCENT, 1), INPUT_KEY(G, 1)}},
};

/*
 * The sky at night: the Hillaire atmosphere with the sun well under the
 * horizon, the moon and the stars up, and the IBL baked from it -- which is
 * what lights the street, and what the fog's ambient follows. apps/tree's
 * setup, with this world's metre.
 */
static void build_sky(Engine* engine) {
    SkyAtmosphere* sky = create_sky_atmosphere();
    IBLResources* ibl = create_ibl_resources();
    if (!sky || !ibl)
        return;
    sky->world_units_per_km = 1000.0f;
    if (g_args.day) {
        // Overcast noon is the fog's own look; the sun is high and weak.
        sky->sun_elevation_deg = 35.0f;
        sky->sun_azimuth_deg = 150.0f;
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
    if (sky_bake_static_luts(sky, engine) != 0 || sky_bake(sky, ibl, engine) != 0)
        return;
    g_scene->sky = sky;
    g_scene->ibl = ibl;
    g_scene->render_skybox = true;
    g_scene->skybox_brightness = 1.0f;
    g_scene->skybox_ground_projection = false;

    LightDesc sun_desc = {
        .name = "sun", .type = LIGHT_DIRECTIONAL, .size = {4.0f, 4.0f}, .cast_shadows = true};
    Light* sun = create_light(&sun_desc);
    sky->sun_light = sun;
    sky->sun_base_intensity = g_args.day ? 3.0f : 0.0f;
    sky_apply_sun_to_light(sky);
    scene_add_light(g_scene, sun);
    SceneNode* sun_node = create_node();
    node_set_name(sun_node, "sun");
    node_set_light(sun_node, sun);
    node_add_child(g_scene->root_node, sun_node);

    LightDesc moon_desc = {.name = "moon", .type = LIGHT_DIRECTIONAL, .size = {4.0f, 4.0f}};
    Light* moon = create_light(&moon_desc);
    sky->moon_light = moon;
    sky_update_moon(sky); // owns its direction, tint, intensity and shadows
    scene_add_light(g_scene, moon);
    SceneNode* moon_node = create_node();
    node_set_name(moon_node, "moon");
    node_set_light(moon_node, moon);
    node_add_child(g_scene->root_node, moon_node);
}

/*
 * Irradiance probes over the house, so a room is lit by what it can see: the
 * tubes' light off its own walls, and not the sky, which the environment's
 * irradiance would otherwise pour into every closed room through the roof.
 *
 * The grid is placed so no probe centre lands in a wall. Probes sit at cell
 * centres, so with a one-metre cell across X from -5.75 the columns fall at
 * -5.25 ... 5.75, a quarter metre off every wall line (-5, -1.5, 0, 5); across
 * Z from 8.1 the rows fall at 8.6 ... 16.6, clear of 10, 13.8 and 16. A probe
 * inside a wall sees only backfaces and darkens everything near it.
 *
 * Outside the grid a query clamps to the nearest edge probes, which stand in
 * the front yard -- the right kind of answer for the street, which is lit
 * mostly by its own lamps and the moon rather than by what bounces.
 */
static void build_gi(void) {
    GIVolume* gi = create_gi_volume(12, 3, 9);
    if (!gi)
        return;
    gi_volume_fit(gi, (vec3){-5.75f, FLOOR_Y, 8.1f}, (vec3){6.25f, CEIL_Y, 17.1f});
    g_scene->gi_volume = gi;
}

/*
 * Reflection probes in the kitchen and the hall. Without them every metal and
 * every wet surface indoors reflects the only environment there is, the night
 * sky, and the hood, the sink and the floor go black. Captured once, like the
 * irradiance probes, and after them: the two share an atlas, which the probes
 * allocate with the volume's columns reserved.
 */
static void build_probes(Engine* engine) {
    if (!g_scene->ibl || !g_scene->ibl->precomputed)
        return;
    const struct {
        vec3 pos, lo, hi;
    } rooms[] = {
        {{2.48f, FLOOR_Y + 1.5f, 11.9f},
         {KITCHEN_X0, FLOOR_Y, KITCHEN_Z0},
         {KITCHEN_X1, CEIL_Y, KITCHEN_Z1}},
        {{-0.75f, FLOOR_Y + 1.5f, 13.0f},
         {HALL_X0 + 0.5f * INT_WALL, FLOOR_Y, HOUSE_FRONT_Z + 0.5f * EXT_WALL},
         {HALL_X1 - 0.5f * INT_WALL, CEIL_Y, HOUSE_BACK_Z - 0.5f * EXT_WALL}},
    };
    ReflectionProbeSet* set = create_reflection_probe_set();
    if (!set)
        return;
    float near_clips[2], far_clips[2];
    const bool env_only[2] = {false, false};
    for (int i = 0; i < 2; i++) {
        ReflectionProbe* p = create_reflection_probe();
        if (!p)
            break;
        glm_vec3_copy((float*)rooms[i].pos, p->position);
        glm_vec3_copy((float*)rooms[i].lo, p->box_min);
        glm_vec3_copy((float*)rooms[i].hi, p->box_max);
        vec3 span;
        glm_vec3_sub(p->box_max, p->box_min, span);
        const float radius = 0.5f * glm_vec3_norm(span);
        near_clips[set->count] = 0.02f;
        far_clips[set->count] = 4.0f * radius;
        if (!probe_set_add(set, p)) {
            free_reflection_probe(p);
            break;
        }
    }
    if (set->count == 2 &&
        probe_set_capture_all(set, engine, g_scene, near_clips, far_clips, env_only, 0))
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
    // The film look warms the highlights; this place is green-grey, and the
    // grade that makes it so is the LUT (tools/make_grade.py).
    glm_vec3_one(fx->grade_gain);
    if (grade && postfx_load_lut(fx, "assets/lut/silent_grade.cube"))
        fx->lut_strength = 1.0f;

    // A thin haze everywhere -- enough to put a beam in the flashlight and a
    // halo round a lamp -- and the street's fog volumes on top of it.
    fx->fog_enabled = true;
    fx->fog_density = 0.02f;
    fx->fog_height_falloff = 60.0f;
    fx->fog_floor_y = 0.0f;
    fx->fog_far = 60.0f;
    fx->fog_anisotropy = 0.7f;
    // The fog's own glow, and not the sky's: the night sky's radiance is a
    // deep navy, and fog lit by it swallows the street into black. A dim
    // grey-green veil is what the houses fade INTO, which is what reads as fog.
    postfx_set_fog_ambient(fx, night ? (vec3){5.0f, 5.6f, 5.2f} : (vec3){400.0f, 420.0f, 410.0f});
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
    if (!mats_register(&kit, engine, g_scene))
        return;
    house_build(&kit);
    kitchen_build(&kit, (unsigned int)g_args.seed);
    // The tubes and the flashlight before the street's lamps: the fog beams
    // the scene's first spot, and it should be the one in the player's hand.
    lights_build(&g_lights, &kit, engine, g_scene, (unsigned int)g_args.seed, !g_args.no_flicker,
                 g_args.flashlight != 0);
    street_build(&kit, g_scene, (unsigned int)g_args.seed, !g_args.day);
    kit_finish(&kit, "world");
    printf("silent: %d colliders\n", kit.collider_count);

    build_sky(engine);

    ShadowSystem* ss = g_scene->shadow_system;
    if (ss) {
        ss->enabled = true;
        ss->ortho_size = 60.0f;
        ss->near_plane = 0.1f;
        ss->far_plane = 200.0f;
        ss->shadow_distance = 40.0f;
        ss->cascade_count = 2;
        ss->pcss_enabled = true;
    }

    CameraDesc cam = {.position = {SPAWN_FEET[0], SPAWN_FEET[1] + PLAYER_EYE_HEIGHT, SPAWN_FEET[2]},
                      .look_at = {SPAWN_FEET[0], SPAWN_FEET[1] + PLAYER_EYE_HEIGHT, 0.0f},
                      .fov = glm_rad(g_args.fov_deg > 0.0f ? g_args.fov_deg : 68.0f),
                      .near = 0.05f,
                      .far = 250.0f};
    engine_set_camera(engine, create_camera(&cam));

    player_init(&g_player, game, physics, em, SPAWN_FEET, SPAWN_YAW);
    physics_world_optimize(physics);

    // Pinned: a meter would open the dark back up, which is the one thing
    // this place must not do.
    engine->exposure.automatic = false;
    engine->exposure.multiplier = 0.02f;

    build_post(engine, !g_args.day, !g_args.no_grade);
}

static void on_update(Game* game, double dt) {
    player_update(&g_player, game, dt);
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

static void on_pre_render(Game* game, double alpha) {
    (void)alpha;
    Engine* engine = game->engine;
    if (input_action_pressed(&game->input, "toggle_gui"))
        engine->show_gui = !engine->show_gui;
    const bool pinned = g_args.cam_eye_set && g_args.cam_target_set;
    player_pre_render(&g_player, game, pinned ? &g_args.cam_eye : NULL,
                      pinned ? &g_args.cam_target : NULL);

    if (input_action_pressed(&game->input, "flashlight"))
        lights_toggle_flashlight(&g_lights);
    vec3 eye = {0.0f, 0.0f, 0.0f}, forward = {0.0f, 0.0f, -1.0f};
    player_eye(&g_player, eye, forward);
    lights_update(&g_lights, g_scene, game->time, (float)game->sim_clock.delta, eye, forward);

    // The probes go in on the third frame, not at load. The tubes' panels are
    // derived during the first frame's draw and only cast from the next, and a
    // volume's FIRST sweep is the only one taken at full weight -- a re-arm
    // blends into what is already there -- so it has to see the lit room.
    if (engine->total_frames == 2 && !g_scene->gi_volume) {
        build_gi();
        build_probes(engine);
    }
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
    printf("      --day               Overcast day in the fog instead of night\n");
    printf("      --no-taa            No temporal AA: the exactly repeatable frame\n");
    printf("      --no-grade          Without the green-grey colour grade\n");
    printf("      --render-scale F    Render at F of the window (0.5-1) and upscale: softer,\n");
    printf("                          lower-res, more like the consoles it imitates\n");
    printf("      --msaa N            MSAA samples\n");
    printf("      --cam-eye x,y,z     Pin the camera (a framing that can be taken twice)\n");
    printf("      --cam-target x,y,z  What the pinned camera looks at\n");
    printf("      --fov D             Vertical field of view, degrees (default 68)\n");
    printf("      --pad-script PATH   Replay a scripted pad on slot 0 (see input.h)\n");
    printf("      --trace-player      Print the player's position every 30 steps\n");
    printf("      --no-flicker        Keep the failing ceiling tube steady\n");
    printf("      --flashlight        Start with the flashlight on (F toggles it)\n");
    printf("  In the window: click to capture the mouse, Tab to release it. WASD\n");
    printf("  walks, Shift hurries, the arrows or the mouse look, G shows the GUI.\n");
    printf("  -h, --help              This message\n");
}

static bool parse_args(int argc, char** argv, SilentArgs* a) {
    memset(a, 0, sizeof(*a));
    a->width = DEFAULT_WIDTH;
    a->height = DEFAULT_HEIGHT;
    a->seed = 7;
    for (int i = 1; i < argc; i++) {
        const char* s = argv[i];
        const bool has_next = i + 1 < argc;
        if (!strcmp(s, "-x") || !strcmp(s, "--headless")) {
            a->headless = 1;
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
            a->day = 1;
        } else if (!strcmp(s, "--no-taa")) {
            a->no_taa = 1;
        } else if (!strcmp(s, "--no-grade")) {
            a->no_grade = 1;
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
            a->trace_player = 1;
        } else if (!strcmp(s, "--no-flicker")) {
            a->no_flicker = 1;
        } else if (!strcmp(s, "--flashlight")) {
            a->flashlight = 1;
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
    return true;
}

int main(int argc, char** argv) {
    if (!parse_args(argc, argv, &g_args))
        return 1;

    GameConfig config = {.engine = {.title = "silent",
                                    .width = g_args.width,
                                    .height = g_args.height,
                                    .headless = g_args.headless != 0}};
    // One sample under TAA, headless as in the window, so a screenshot is what
    // a player sees. Not MSAA: in fog this dense the fog composite takes ONE
    // depth for a multisampled edge pixel, and where half its samples are sky
    // that depth is too near -- every silhouette in the street comes out
    // traced in a dark, unfogged line.
    config.engine.msaa_samples = 1;
    config.engine.taa = !g_args.no_taa;
    // And jittered, or headless TAA integrates one sample position forever:
    // no antialiasing, and an upscale the engine refuses. --no-taa is the
    // exactly repeatable frame.
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

    // One table, the player's actions and the app's, because input_bind
    // replaces rather than appends. Static: the binding borrows it.
    static InputAction actions[32];
    int n = 0;
    for (int i = 0; i < PLAYER_ACTION_COUNT && n < 32; i++)
        actions[n++] = PLAYER_ACTIONS[i];
    for (size_t i = 0; i < sizeof(APP_ACTIONS) / sizeof(APP_ACTIONS[0]) && n < 32; i++)
        actions[n++] = APP_ACTIONS[i];
    input_bind(&game->input, actions, (size_t)n);
    if (g_args.pad_script && !input_set_pad_script(&game->input, g_args.pad_script)) {
        free_game(game);
        return 1;
    }

    game_set_init(game, on_init);
    game_set_update(game, on_update);
    game_set_pre_render(game, on_pre_render);
    game_run(game);
    free_game(game);
    return 0;
}
