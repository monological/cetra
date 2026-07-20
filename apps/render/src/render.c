#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "cetra/common.h"
#include "cetra/mesh.h"
#include "cetra/shader.h"
#include "cetra/program.h"
#include "cetra/scene.h"
#include "cetra/util.h"
#include "cetra/engine.h"
#include "cetra/import.h"
#include "cetra/render.h"
#include "cetra/transform.h"
#include "cetra/animation.h"
#include "cetra/springbone.h"
#include "cetra/ibl.h"
#include "cetra/sky.h"
#include "cetra/app.h"

#include "cetra/shader_strings.h"

/*
 * Constants
 */
#define DEFAULT_WIDTH 1920
#define DEFAULT_HEIGHT 1080
#define MAX_ANIM_FILES 32

const float MIN_DIST = 2000.0f;
const float MAX_DIST = 3000.0f;
const float CAM_ANGULAR_SPEED = 0.5f;

// Total analytic key-light intensity split across the HDR's light lobes.
// Studio flashes overpower ambient by a few stops; matching that is what
// makes dark, rough materials (near-black armor) read instead of flattening
const float KEY_LIGHT_TOTAL_INTENSITY = 10.0f;

/*
 * Command line arguments
 */
typedef struct {
    const char* model_path;
    const char* texture_dir;
    const char* hdr_path;
    const char* anim_files[MAX_ANIM_FILES];
    int anim_count;
    const char* source_skeleton_path;  // Source skeleton for retargeting
    const char* screenshot_path;       // Save final frame here (PPM)
    int screenshot_every;              // Also save numbered frames every N frames
    float fov_deg;                     // Camera FOV in degrees (0 = default 50)
    float exposure;                    // Tonemap exposure override (0 = engine default)
    float ground_radius;               // Skybox ground projection dome radius (0 = default)
    float ground_height;               // HDR capture height above ground (0 = default)
    float camera_distance;             // Camera distance override in meters (0 = auto)
    int no_recenter;                   // Keep the model's authored world position
    int no_auto_exposure;              // Fixed exposure instead of eye adaptation
    int no_flip_uv;                    // For assets baked with the opposite V convention
    float ao_radius;                   // AO/GI reach override in world units (0 = auto)
    int force_taa;                     // TAA even in headless (temporal passes active)
    int no_ground;                     // Disable skybox ground projection
    int no_key_light;                  // Pure IBL: skip the analytic key lights
    int no_shadows;                    // Keep key lights but disable shadow maps
    int no_pcss;                       // Fixed-width PCF instead of contact-hardening
    float light_size;                  // Emitter size override (-1 = scene default)
    float shadow_softness;             // PCSS softness override (-1 = default)
    int shadow_cascades;               // Cascades per caster (0 = keep engine default)
    int csm_debug;                     // Tint fragments by selected cascade
    int no_springs;                    // Disable spring-bone secondary motion
    int no_ssao;                       // Disable screen-space ambient occlusion
    int ssao_debug;                    // Show the raw SSAO buffer
    int no_spec_occlusion;             // Let GTAO darken specular (disable spec-occ)
    int no_ao_edge_filter;             // Disable the depth-aware AO blur (allow silhouette bleed)
    int ssgi;                          // Enable screen-space GI (indirect diffuse)
    int ssgi_debug;                    // Show the raw gathered GI radiance
    int probe;                         // Enable the local reflection probe
    int probe_pos_set;                 // --probe-pos given
    float probe_pos[3];                // Probe capture position override
    int probe_scene;                   // Capture the scene meshes too (interiors)
    int probe_debug;                   // Show the raw capture as the background
    int sky;                           // Procedural physically-based sky instead of -e
    int sky_debug;                     // Blit the sky LUTs into the frame corner
    float sun_elevation;               // Sky sun elevation in degrees (-999 = default)
    float sun_azimuth;                 // Sky sun azimuth in degrees (-999 = default)
    int sky_rebake_stress;             // Diagnostic: N headless sun re-bakes then restore
    int fog;                           // Enable volumetric fog
    int fog_debug;                     // Show the raw fog in-scatter buffer
    float fog_density;                 // Extinction override (0 = scene-scaled)
    float fog_height;                  // Height falloff override (0 = scene-scaled)
    int albedo_debug;                  // Show the resolved albedo G-buffer
    int no_normals_mrt;                // Disable the normals G-buffer
    int normals_debug;                 // Show the resolved normals G-buffer
    int no_ssr;                        // Disable screen-space reflections
    int no_ssr_full_res;               // Trace SSR at half res (the old, serrated path)
    int no_ssr_temporal;               // Disable SSR temporal accumulation (raw single-frame march)
    int no_ssr_denoise;                // Disable the SSR denoiser (deterministic march, no jitter)
    float ssr_jitter;                  // SSR stochastic ray-jitter spread override (-1 = default)
    int ssr_debug;                     // Show the reflection buffer
    float ssr_strength;                // SSR strength override (-1 = default)
    float specular_aa;                 // Specular AA strength override (-1 = default)
    int no_energy_comp;                // Disable multi-scatter energy compensation
    int no_refraction;                 // Disable screen-space refraction
    int no_clearcoat;                  // Disable the clearcoat second specular lobe
    int no_specular;                   // Disable KHR_materials_specular F0 tint + weight
    int no_sheen;                      // Disable KHR_materials_sheen cloth lobe
    int no_parallax;                   // Disable parallax occlusion mapping (POM)
    float parallax_scale;              // POM depth override (< 0 = keep engine default)
    int no_sss;                        // Disable separable subsurface scattering
    float sss_radius;                  // SSS scatter radius override (< 0 = fixture default)
    float sss_color[3];                // SSS scatter color override (< 0 in [0] = fixture default)
    int no_bloom;                      // Disable bloom
    int tonemap_mode;                  // PostFXTonemapMode override (0 = keep default;
                                       // coincides with PASSTHROUGH, which is a blit
                                       // path and never user-set)
    int ssaa;                          // Supersampling factor (0 = keep engine default)
    // Finishing grade (-1 = keep engine default; >=0 enables + sets)
    int film_preset; // --film: enable the whole finishing stack at sane defaults
    float vignette;
    int no_vignette; // Force the default vignette off
    float grain;
    float sharpen;
    int grade_set; // A grade component was given -> enable the grade
    float grade_lift[3];
    float grade_gamma[3];
    float grade_gain[3];
    int dof;         // --dof: enable depth of field
    int no_dof;      // Force DoF off (e.g. --film --no-dof)
    float dof_focus; // Focus distance in view units (-1 = auto: subject)
    float dof_range; // Ramp-to-full-blur width (-1 = scene-scaled default)
    float dof_max_coc;
    int motion_blur;         // --motion-blur: enable motion blur
    float motion_blur_scale; // --motion-blur-scale shutter (-1 = engine default)
    int width;
    int height;
    int headless;
    int headless_jitter;               // Apply TAA jitter in headless (lets temporal effects converge)
    int max_frames; // Exit after this many frames (0 = run forever)
    int show_bones;
    int check_stretch; // One-shot CPU skinning stretch diagnostic
    int render_mode;   // RenderMode override for debugging (-1 = PBR)
    float orbit_yaw;   // Camera yaw around the model in degrees (0 = front)
    float orbit_pitch; // Camera pitch in degrees (0 = level, 90 = top-down)
    // Explicit camera pose (reproduces any interactive view; overrides the
    // yaw/pitch/distance orbit framing). --cam-eye and --cam-target must both
    // be given; --cam-up is optional (default 0,1,0). Print them from the GUI
    // "Print Camera" button.
    int cam_eye_set;    // --cam-eye given
    float cam_eye[3];   // Explicit eye position (world)
    int cam_target_set; // --cam-target given
    float cam_target[3];// Explicit look-at target (world)
    int cam_up_set;     // --cam-up given
    float cam_up[3];    // Explicit up vector (default 0,1,0)
    int show_help;
} RenderArgs;

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s -m <model> [options]\n\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -m, --model <path>     Model file (FBX, glTF, OBJ) [required]\n");
    fprintf(stderr, "  -t, --textures <dir>   Texture directory\n");
    fprintf(stderr, "  -e, --env <path>       HDR environment map for IBL\n");
    fprintf(stderr, "  -F, --fov <degrees>    Camera field of view (default: 50)\n");
    fprintf(stderr, "      --cam-eye x,y,z    Explicit camera position (exact-repro; needs --cam-target)\n");
    fprintf(stderr, "      --cam-target x,y,z Explicit look-at target (overrides --yaw/--pitch/--distance)\n");
    fprintf(stderr, "      --cam-up x,y,z     Explicit up vector (default: 0,1,0)\n");
    fprintf(stderr, "  -E, --exposure <f>     Fixed tonemap exposure (disables auto-exposure)\n");
    fprintf(stderr, "      --no-auto-exposure Fixed exposure instead of eye adaptation\n");
    fprintf(stderr, "      --ground <radius>  Ground projection dome radius (default: 5x scene)\n");
    fprintf(stderr, "      --no-recenter      Keep the model's authored world position\n");
    fprintf(stderr, "      --no-flip-uv       For assets baked with the opposite UV V convention\n");
    fprintf(stderr, "                         (symptom: scrambled/mirrored textures)\n");
    fprintf(stderr, "      --taa              Enable TAA in headless (temporal passes active;\n");
    fprintf(stderr, "                         output not byte-deterministic)\n");
    fprintf(stderr, "      --ground-height <m> HDR capture height above ground (default: 1.2)\n");
    fprintf(stderr, "      --no-ground        Disable HDR ground projection (infinite skybox)\n");
    fprintf(stderr, "      --no-key-light     Pure IBL lighting (no analytic lights/shadows)\n");
    fprintf(stderr, "      --no-shadows       Keep key lights but disable shadow maps\n");
    fprintf(stderr, "      --no-pcss          Fixed-width PCF instead of contact-hardening\n");
    fprintf(stderr, "      --light-size <f>   Emitter size for penumbra (default: scene-scaled)\n");
    fprintf(stderr, "      --shadow-softness <f> PCSS softness multiplier (default: 1)\n");
    fprintf(stderr, "      --shadow-cascades <n> Shadow cascades per caster, 1-3 (default: 3)\n");
    fprintf(stderr, "      --csm-debug        Tint fragments by selected shadow cascade\n");
    fprintf(stderr, "      --no-springs       Disable spring-bone secondary motion\n");
    fprintf(stderr, "      --no-ssao          Disable screen-space ambient occlusion\n");
    fprintf(stderr, "      --ssao-debug       Show the raw SSAO buffer\n");
    fprintf(stderr, "      --no-spec-occlusion Let GTAO darken specular (disable spec-occlusion)\n");
    fprintf(stderr, "      --no-ao-edge-filter Disable the depth-aware AO blur\n");
    fprintf(stderr, "      --ao-radius <f>    AO/GI reach in world units (default: scene-scaled)\n");
    fprintf(stderr, "      --no-normals-mrt   Disable the normals G-buffer (SSAO/SSR input)\n");
    fprintf(stderr, "      --normals-debug    Show the resolved normals G-buffer\n");
    fprintf(stderr, "      --no-ssr           Disable screen-space reflections\n");
    fprintf(stderr, "      --no-ssr-full-res  Trace SSR at half res (old serrated path)\n");
    fprintf(stderr, "      --no-ssr-temporal  Disable SSR temporal accumulation (needs TAA)\n");
    fprintf(stderr, "      --no-ssr-denoise   Disable the SSR denoiser (deterministic march)\n");
    fprintf(stderr, "      --ssr-jitter <f>   SSR stochastic ray-jitter spread (default: 0.03)\n");
    fprintf(stderr, "      --ssr-debug        Show the reflection buffer\n");
    fprintf(stderr, "      --ssr-strength <f> Reflection strength (default: 1)\n");
    fprintf(stderr, "      --ssgi             Enable screen-space GI (one-bounce indirect diffuse)\n");
    fprintf(stderr, "      --ssgi-debug       Show the raw gathered GI radiance (implies --ssgi)\n");
    fprintf(stderr, "      --probe            Local reflection probe: grounded env reflections + SSR fallback\n");
    fprintf(stderr, "      --probe-pos x,y,z  Probe parallax origin (implies --probe; default: auto)\n");
    fprintf(stderr, "      --probe-scene      Capture the scene meshes into the probe (interiors)\n");
    fprintf(stderr, "      --probe-debug      Show the raw capture as the background (implies --probe)\n");
    fprintf(stderr, "      --sky              Procedural physically-based sky (instead of -e)\n");
    fprintf(stderr, "      --sky-debug        Blit the sky LUTs into the frame corner\n");
    fprintf(stderr, "      --sun-elevation <d> Sky sun elevation in degrees (implies --sky)\n");
    fprintf(stderr, "      --sun-azimuth <d>  Sky sun azimuth in degrees (implies --sky)\n");
    fprintf(stderr, "      --fog              Volumetric fog: god rays + height haze\n");
    fprintf(stderr, "      --fog-debug        Show the raw fog in-scatter buffer (implies --fog)\n");
    fprintf(stderr, "      --fog-density <f>  Fog extinction per world unit (implies --fog)\n");
    fprintf(stderr, "      --fog-height <f>   Fog height falloff in world units (implies --fog)\n");
    fprintf(stderr, "      --albedo-debug     Show the resolved albedo G-buffer\n");
    fprintf(stderr, "      --specular-aa <f>  Specular anti-aliasing strength (default: 1)\n");
    fprintf(stderr, "      --no-specular-aa   Disable specular anti-aliasing\n");
    fprintf(stderr, "      --no-energy-comp   Disable multi-scatter specular energy comp\n");
    fprintf(stderr, "      --no-refraction    Disable screen-space refraction\n");
    fprintf(stderr, "      --no-clearcoat     Disable the clearcoat specular lobe\n");
    fprintf(stderr, "      --no-specular      Disable KHR_materials_specular (F0 tint + weight)\n");
    fprintf(stderr, "      --no-sheen         Disable KHR_materials_sheen (cloth lobe)\n");
    fprintf(stderr, "      --no-parallax      Disable parallax occlusion mapping (POM)\n");
    fprintf(stderr, "      --parallax-scale <f> POM depth (default 0.05; 0 = off)\n");
    fprintf(stderr, "      --no-sss           Disable separable subsurface scattering\n");
    fprintf(stderr, "      --sss-radius <f>   SSS scatter radius (world units)\n");
    fprintf(stderr, "      --sss-color <r,g,b> SSS per-channel scatter color (e.g. 1.0,0.3,0.2)\n");
    fprintf(stderr, "      --no-bloom         Disable bloom\n");
    fprintf(stderr, "      --tonemap <m>      Tonemap mode: aces, neutral, agx (default: neutral)\n");
    fprintf(stderr, "      --ssaa <int>       Supersampling factor (default: 1 = off; 2 = 2x SSAA)\n");
    fprintf(stderr, "      --no-ssaa          Disable supersampling (render at 1x)\n");
    fprintf(stderr, "      --film             Cinematic finish preset (vignette+grain+sharpen+grade)\n");
    fprintf(stderr, "      --vignette <s>     Vignette strength (enables it; default on ~0.25)\n");
    fprintf(stderr, "      --no-vignette      Disable the default vignette\n");
    fprintf(stderr, "      --grain <s>        Film grain strength (enables it)\n");
    fprintf(stderr, "      --sharpen <s>      Unsharp-mask strength (enables it)\n");
    fprintf(stderr, "      --grade-lift/gamma/gain r,g,b  Colour grade (enables it)\n");
    fprintf(stderr, "      --dof              Depth of field, autofocused on the subject\n");
    fprintf(stderr, "      --no-dof           Force depth of field off (e.g. with --film)\n");
    fprintf(stderr, "      --dof-focus <m>    Pin focus distance (disables autofocus)\n");
    fprintf(stderr, "      --dof-range <m>    Distance over which blur ramps in (default: auto)\n");
    fprintf(stderr, "      --dof-max-coc <px> Max blur radius in half-res texels (default: 6)\n");
    fprintf(stderr, "      --motion-blur      Velocity-buffer motion blur (McGuire reconstruction)\n");
    fprintf(stderr, "      --motion-blur-scale <s> Shutter/velocity multiplier (default: 1)\n");
    fprintf(stderr, "  -D, --distance <m>     Camera distance from model (default: auto)\n");
    fprintf(stderr, "  -a, --anim <path>      Animation file (can be repeated)\n");
    fprintf(stderr, "  -s, --source <path>    Source skeleton for retargeting (T-pose)\n");
    fprintf(stderr, "  -W, --width <int>      Window width (default: %d)\n", DEFAULT_WIDTH);
    fprintf(stderr, "  -H, --height <int>     Window height (default: %d)\n", DEFAULT_HEIGHT);
    fprintf(stderr, "  -x, --headless         Hidden window (for debugging/CI)\n");
    fprintf(stderr, "      --headless-jitter  Apply TAA sub-pixel jitter in headless (needs --taa;\n");
    fprintf(stderr, "                         non-deterministic, but converges temporal SSR/AA)\n");
    fprintf(stderr, "  -b, --show-bones       Enable bone X-ray overlay\n");
    fprintf(stderr, "      --check-stretch    Report triangle edges stretched by skinning\n");
    fprintf(stderr, "  -f, --frames <int>     Exit after N frames\n");
    fprintf(stderr, "  -S, --screenshot <path> Save final frame as PPM on exit\n");
    fprintf(stderr, "      --screenshot-every <N> Also save numbered frames every N frames\n");
    fprintf(stderr, "  -h, --help             Show this help message\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s -m character.fbx -t textures/\n", prog);
    fprintf(stderr, "  %s -m character.fbx -a walk.fbx -s mixamo_tpose.fbx\n", prog);
}

static int parse_args(int argc, char** argv, RenderArgs* args) {
    memset(args, 0, sizeof(RenderArgs));
    args->width = DEFAULT_WIDTH;
    args->height = DEFAULT_HEIGHT;
    args->specular_aa = -1.0f;    // -1 = keep the engine default
    args->ssr_strength = -1.0f;   // -1 = keep the engine default
    args->ssr_jitter = -1.0f;     // -1 = keep the engine default
    args->parallax_scale = -1.0f; // -1 = keep the engine default POM depth
    args->sss_radius = -1.0f;     // -1 = keep the fixture default SSS radius
    args->sss_color[0] = -1.0f;   // -1 = keep the fixture default SSS scatter color
    args->vignette = -1.0f;
    args->grain = -1.0f;
    args->sharpen = -1.0f;
    args->grade_gamma[0] = args->grade_gamma[1] = args->grade_gamma[2] = 1.0f;
    args->grade_gain[0] = args->grade_gain[1] = args->grade_gain[2] = 1.0f;
    args->dof_focus = -1.0f;
    args->dof_range = -1.0f;
    args->dof_max_coc = -1.0f;
    args->motion_blur_scale = -1.0f;
    args->light_size = -1.0f;     // -1 = scene-radius default
    args->shadow_softness = -1.0f; // -1 = keep the engine default
    args->sun_elevation = -999.0f; // -999 = keep the sky default
    args->sun_azimuth = -999.0f;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            args->show_help = 1;
            return 0;
        } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--model") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->model_path = argv[i];
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--textures") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->texture_dir = argv[i];
        } else if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--env") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->hdr_path = argv[i];
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--anim") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            if (args->anim_count >= MAX_ANIM_FILES) {
                fprintf(stderr, "Error: too many animation files (max %d)\n", MAX_ANIM_FILES);
                return -1;
            }
            args->anim_files[args->anim_count++] = argv[i];
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--source") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->source_skeleton_path = argv[i];
        } else if (strcmp(argv[i], "-W") == 0 || strcmp(argv[i], "--width") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->width = atoi(argv[i]);
            if (args->width <= 0) {
                fprintf(stderr, "Error: invalid width '%s'\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "-H") == 0 || strcmp(argv[i], "--height") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->height = atoi(argv[i]);
            if (args->height <= 0) {
                fprintf(stderr, "Error: invalid height '%s'\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "-x") == 0 || strcmp(argv[i], "--headless") == 0) {
            args->headless = 1;
        } else if (strcmp(argv[i], "--headless-jitter") == 0) {
            args->headless_jitter = 1;
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--show-bones") == 0) {
            args->show_bones = 1;
        } else if (strcmp(argv[i], "--check-stretch") == 0) {
            args->check_stretch = 1;
        } else if (strcmp(argv[i], "-F") == 0 || strcmp(argv[i], "--fov") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->fov_deg = (float)atof(argv[i]);
            if (args->fov_deg <= 0.0f || args->fov_deg >= 180.0f) {
                fprintf(stderr, "Error: invalid fov '%s' (use 1-179 degrees)\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "--render-mode") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->render_mode = atoi(argv[i]);
        } else if (strcmp(argv[i], "--yaw") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->orbit_yaw = (float)atof(argv[i]);
        } else if (strcmp(argv[i], "--pitch") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->orbit_pitch = (float)atof(argv[i]);
        } else if (strcmp(argv[i], "--cam-eye") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            if (sscanf(argv[i], "%f,%f,%f", &args->cam_eye[0], &args->cam_eye[1],
                       &args->cam_eye[2]) != 3) {
                fprintf(stderr, "Error: --cam-eye expects x,y,z\n");
                return -1;
            }
            args->cam_eye_set = 1;
        } else if (strcmp(argv[i], "--cam-target") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            if (sscanf(argv[i], "%f,%f,%f", &args->cam_target[0], &args->cam_target[1],
                       &args->cam_target[2]) != 3) {
                fprintf(stderr, "Error: --cam-target expects x,y,z\n");
                return -1;
            }
            args->cam_target_set = 1;
        } else if (strcmp(argv[i], "--cam-up") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            if (sscanf(argv[i], "%f,%f,%f", &args->cam_up[0], &args->cam_up[1],
                       &args->cam_up[2]) != 3) {
                fprintf(stderr, "Error: --cam-up expects x,y,z\n");
                return -1;
            }
            args->cam_up_set = 1;
        } else if (strcmp(argv[i], "-E") == 0 || strcmp(argv[i], "--exposure") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->exposure = (float)atof(argv[i]);
            if (args->exposure <= 0.0f) {
                fprintf(stderr, "Error: invalid exposure '%s'\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "--no-ground") == 0) {
            args->no_ground = 1;
        } else if (strcmp(argv[i], "--no-recenter") == 0) {
            args->no_recenter = 1;
        } else if (strcmp(argv[i], "--no-auto-exposure") == 0) {
            args->no_auto_exposure = 1;
        } else if (strcmp(argv[i], "--no-flip-uv") == 0) {
            args->no_flip_uv = 1;
        } else if (strcmp(argv[i], "--ao-radius") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->ao_radius = (float)atof(argv[i]);
            if (args->ao_radius <= 0.0f) {
                fprintf(stderr, "Error: invalid AO radius '%s'\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "--taa") == 0) {
            args->force_taa = 1;
        } else if (strcmp(argv[i], "--ground") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->ground_radius = (float)atof(argv[i]);
            if (args->ground_radius <= 0.0f) {
                fprintf(stderr, "Error: invalid ground radius '%s'\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "--ground-height") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->ground_height = (float)atof(argv[i]);
            if (args->ground_height <= 0.0f) {
                fprintf(stderr, "Error: invalid ground height '%s'\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "--no-key-light") == 0) {
            args->no_key_light = 1;
        } else if (strcmp(argv[i], "--no-shadows") == 0) {
            args->no_shadows = 1;
        } else if (strcmp(argv[i], "--no-pcss") == 0) {
            args->no_pcss = 1;
        } else if (strcmp(argv[i], "--light-size") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->light_size = (float)atof(argv[i]);
        } else if (strcmp(argv[i], "--shadow-softness") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->shadow_softness = (float)atof(argv[i]);
        } else if (strcmp(argv[i], "--shadow-cascades") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->shadow_cascades = atoi(argv[i]);
            if (args->shadow_cascades < 1 || args->shadow_cascades > SHADOW_CASCADES) {
                fprintf(stderr, "Error: --shadow-cascades must be 1..%d\n", SHADOW_CASCADES);
                return -1;
            }
        } else if (strcmp(argv[i], "--csm-debug") == 0) {
            args->csm_debug = 1;
        } else if (strcmp(argv[i], "--no-ssao") == 0) {
            args->no_ssao = 1;
        } else if (strcmp(argv[i], "--ssao-debug") == 0) {
            args->ssao_debug = 1;
        } else if (strcmp(argv[i], "--no-spec-occlusion") == 0) {
            args->no_spec_occlusion = 1;
        } else if (strcmp(argv[i], "--no-ao-edge-filter") == 0) {
            args->no_ao_edge_filter = 1;
        } else if (strcmp(argv[i], "--ssgi") == 0) {
            args->ssgi = 1;
        } else if (strcmp(argv[i], "--ssgi-debug") == 0) {
            args->ssgi = 1;
            args->ssgi_debug = 1;
        } else if (strcmp(argv[i], "--probe") == 0) {
            args->probe = 1;
        } else if (strcmp(argv[i], "--probe-pos") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            if (sscanf(argv[i], "%f,%f,%f", &args->probe_pos[0], &args->probe_pos[1],
                       &args->probe_pos[2]) != 3) {
                fprintf(stderr, "Error: --probe-pos expects x,y,z\n");
                return -1;
            }
            args->probe = 1;
            args->probe_pos_set = 1;
        } else if (strcmp(argv[i], "--probe-scene") == 0) {
            args->probe = 1;
            args->probe_scene = 1;
        } else if (strcmp(argv[i], "--probe-debug") == 0) {
            args->probe = 1;
            args->probe_debug = 1;
        } else if (strcmp(argv[i], "--sky") == 0) {
            args->sky = 1;
        } else if (strcmp(argv[i], "--sky-debug") == 0) {
            args->sky = 1;
            args->sky_debug = 1;
        } else if (strcmp(argv[i], "--sun-elevation") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->sun_elevation = (float)atof(argv[i]);
            args->sky = 1;
        } else if (strcmp(argv[i], "--sun-azimuth") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->sun_azimuth = (float)atof(argv[i]);
            args->sky = 1;
        } else if (strcmp(argv[i], "--sky-rebake-stress") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->sky_rebake_stress = atoi(argv[i]);
            args->sky = 1;
        } else if (strcmp(argv[i], "--fog") == 0) {
            args->fog = 1;
        } else if (strcmp(argv[i], "--fog-debug") == 0) {
            args->fog = 1;
            args->fog_debug = 1;
        } else if (strcmp(argv[i], "--fog-density") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->fog_density = (float)atof(argv[i]);
            args->fog = 1;
        } else if (strcmp(argv[i], "--fog-height") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->fog_height = (float)atof(argv[i]);
            args->fog = 1;
        } else if (strcmp(argv[i], "--albedo-debug") == 0) {
            args->albedo_debug = 1;
        } else if (strcmp(argv[i], "--no-normals-mrt") == 0) {
            args->no_normals_mrt = 1;
        } else if (strcmp(argv[i], "--normals-debug") == 0) {
            args->normals_debug = 1;
        } else if (strcmp(argv[i], "--no-ssr") == 0) {
            args->no_ssr = 1;
        } else if (strcmp(argv[i], "--no-ssr-full-res") == 0) {
            args->no_ssr_full_res = 1;
        } else if (strcmp(argv[i], "--no-ssr-temporal") == 0) {
            args->no_ssr_temporal = 1;
        } else if (strcmp(argv[i], "--no-ssr-denoise") == 0) {
            args->no_ssr_denoise = 1;
        } else if (strcmp(argv[i], "--ssr-jitter") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->ssr_jitter = (float)atof(argv[i]);
        } else if (strcmp(argv[i], "--ssr-debug") == 0) {
            args->ssr_debug = 1;
        } else if (strcmp(argv[i], "--ssr-strength") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->ssr_strength = (float)atof(argv[i]);
        } else if (strcmp(argv[i], "--specular-aa") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->specular_aa = (float)atof(argv[i]);
        } else if (strcmp(argv[i], "--no-specular-aa") == 0) {
            args->specular_aa = 0.0f;
        } else if (strcmp(argv[i], "--no-energy-comp") == 0) {
            args->no_energy_comp = 1;
        } else if (strcmp(argv[i], "--no-refraction") == 0) {
            args->no_refraction = 1;
        } else if (strcmp(argv[i], "--no-clearcoat") == 0) {
            args->no_clearcoat = 1;
        } else if (strcmp(argv[i], "--no-specular") == 0) {
            args->no_specular = 1;
        } else if (strcmp(argv[i], "--no-sheen") == 0) {
            args->no_sheen = 1;
        } else if (strcmp(argv[i], "--no-parallax") == 0) {
            args->no_parallax = 1;
        } else if (strcmp(argv[i], "--parallax-scale") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->parallax_scale = (float)atof(argv[i]);
        } else if (strcmp(argv[i], "--no-sss") == 0) {
            args->no_sss = 1;
        } else if (strcmp(argv[i], "--sss-radius") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->sss_radius = (float)atof(argv[i]);
        } else if (strcmp(argv[i], "--sss-color") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            if (sscanf(argv[i], "%f,%f,%f", &args->sss_color[0], &args->sss_color[1],
                       &args->sss_color[2]) != 3) {
                fprintf(stderr, "Error: --sss-color needs r,g,b (e.g. 1.0,0.3,0.2)\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--no-bloom") == 0) {
            args->no_bloom = 1;
        } else if (strcmp(argv[i], "--tonemap") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            if (strcmp(argv[i], "aces") == 0) {
                args->tonemap_mode = POSTFX_TONEMAP_ACES;
            } else if (strcmp(argv[i], "neutral") == 0) {
                args->tonemap_mode = POSTFX_TONEMAP_NEUTRAL;
            } else if (strcmp(argv[i], "agx") == 0) {
                args->tonemap_mode = POSTFX_TONEMAP_AGX;
            } else {
                fprintf(stderr, "Error: unknown tonemap mode '%s'\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "--ssaa") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->ssaa = atoi(argv[i]);
            if (args->ssaa < 1)
                args->ssaa = 1;
        } else if (strcmp(argv[i], "--no-ssaa") == 0) {
            args->ssaa = 1;
        } else if (strcmp(argv[i], "--film") == 0) {
            args->film_preset = 1;
        } else if (strcmp(argv[i], "--vignette") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->vignette = (float)atof(argv[i]);
        } else if (strcmp(argv[i], "--no-vignette") == 0) {
            args->no_vignette = 1;
        } else if (strcmp(argv[i], "--grain") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->grain = (float)atof(argv[i]);
        } else if (strcmp(argv[i], "--sharpen") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->sharpen = (float)atof(argv[i]);
        } else if (strcmp(argv[i], "--grade-lift") == 0 || strcmp(argv[i], "--grade-gamma") == 0 ||
                   strcmp(argv[i], "--grade-gain") == 0) {
            const char* flag = argv[i];
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an r,g,b argument\n", flag);
                return -1;
            }
            float* dst = strcmp(flag, "--grade-lift") == 0     ? args->grade_lift
                         : strcmp(flag, "--grade-gamma") == 0  ? args->grade_gamma
                                                              : args->grade_gain;
            if (sscanf(argv[i], "%f,%f,%f", &dst[0], &dst[1], &dst[2]) != 3) {
                fprintf(stderr, "Error: %s expects r,g,b (e.g. 1.0,0.95,0.9)\n", flag);
                return -1;
            }
            args->grade_set = 1;
        } else if (strcmp(argv[i], "--dof") == 0) {
            args->dof = 1;
        } else if (strcmp(argv[i], "--no-dof") == 0) {
            args->no_dof = 1;
        } else if (strcmp(argv[i], "--dof-focus") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->dof_focus = (float)atof(argv[i]);
        } else if (strcmp(argv[i], "--dof-range") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->dof_range = (float)atof(argv[i]);
        } else if (strcmp(argv[i], "--dof-max-coc") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->dof_max_coc = (float)atof(argv[i]);
        } else if (strcmp(argv[i], "--motion-blur") == 0) {
            args->motion_blur = 1;
        } else if (strcmp(argv[i], "--motion-blur-scale") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->motion_blur_scale = (float)atof(argv[i]);
        } else if (strcmp(argv[i], "--no-springs") == 0) {
            args->no_springs = 1;
        } else if (strcmp(argv[i], "-D") == 0 || strcmp(argv[i], "--distance") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->camera_distance = (float)atof(argv[i]);
            if (args->camera_distance <= 0.0f) {
                fprintf(stderr, "Error: invalid camera distance '%s'\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "-S") == 0 || strcmp(argv[i], "--screenshot") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->screenshot_path = argv[i];
        } else if (strcmp(argv[i], "--screenshot-every") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->screenshot_every = atoi(argv[i]);
            if (args->screenshot_every <= 0) {
                fprintf(stderr, "Error: invalid screenshot interval '%s'\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--frames") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i - 1]);
                return -1;
            }
            args->max_frames = atoi(argv[i]);
            if (args->max_frames <= 0) {
                fprintf(stderr, "Error: invalid frame count '%s'\n", argv[i]);
                return -1;
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
            return -1;
        } else {
            // Positional argument - treat as model path for backwards compatibility
            if (!args->model_path) {
                args->model_path = argv[i];
            } else if (!args->texture_dir) {
                args->texture_dir = argv[i];
            } else if (!args->hdr_path) {
                args->hdr_path = argv[i];
            } else {
                fprintf(stderr, "Error: unexpected argument '%s'\n", argv[i]);
                return -1;
            }
        }
    }

    if (!args->show_help && !args->model_path) {
        fprintf(stderr, "Error: model path is required\n\n");
        return -1;
    }

    return 0;
}

/*
 * Mouse drag controller
 */
static MouseDragController* drag_controller = NULL;

/*
 * Animation playback state
 */
static AnimationState* anim_state = NULL;
static float last_frame_time = 0.0f;

/*
 * Frame limit (--frames)
 */
static int max_frames = 0;
static int frames_rendered = 0;

/*
 * Stretch diagnostic (--check-stretch)
 */
static int check_stretch = 0;

/*
 * Recenter offset (computed at load): translates the model so its bounding-box
 * base sits on the origin (y=0, centered in x/z). Off-origin assets otherwise
 * misalign with everything anchored at the origin -- the ground-projection
 * dome, the shadow catcher plane, the orbit pivot -- and float geometry far
 * from where the environment projects its floor.
 */
static vec3 model_recenter_offset = {0.0f, 0.0f, 0.0f};

// Bake the recenter offset into the model matrix and node globals. Called
// before every frame's draw and before the load-time probe capture (which
// renders node globals directly); re-application is idempotent.
static void apply_model_recenter(Engine* engine, SceneNode* root_node) {
    Transform transform = {.position = {model_recenter_offset[0], model_recenter_offset[1],
                                        model_recenter_offset[2]},
                           .rotation = {0.0f, 0.0f, 0.0f},
                           .scale = {1.0f, 1.0f, 1.0f}};
    reset_and_apply_transform(&engine->model_matrix, &transform);
    apply_transform_to_nodes(root_node, engine->model_matrix);
}

/*
 * Distance-adaptive near plane (set at load, 0 = disabled). The static
 * scene-scaled near (0.05 x radius, chosen for depth precision) slices into
 * geometry once the camera zooms closer than it; track the camera each frame
 * and shrink near proportionally, clamped to the load-time value so the
 * default framing renders exactly as before.
 */
static float clip_near_max = 0.0f;   // Load-time near (0.05 x scene radius)
static float clip_near_floor = 0.0f; // Lower bound (matches the 10% min zoom)


/*
 * CPU-skin a vertex with the animation state's bone matrices, mirroring the
 * pbr_skinned vertex shader including its identity fallback.
 */
static void cpu_skin_vertex(const Mesh* mesh, const AnimationState* state, size_t v, vec3 out) {
    mat4 skin = GLM_MAT4_ZERO_INIT;
    float total = 0.0f;
    for (int i = 0; i < BONES_PER_VERTEX; i++) {
        int id = mesh->bone_ids[v * BONES_PER_VERTEX + i];
        float w = mesh->bone_weights[v * BONES_PER_VERTEX + i];
        if (id >= 0 && id < MAX_BONES && w > 0.0f) {
            for (int c = 0; c < 4; c++) {
                for (int r = 0; r < 4; r++) {
                    skin[c][r] += state->bone_matrices[id][c][r] * w;
                }
            }
            total += w;
        }
    }
    vec3 pos = {mesh->vertices[v * 3], mesh->vertices[v * 3 + 1], mesh->vertices[v * 3 + 2]};
    if (total < 0.001f) {
        glm_vec3_copy(pos, out);
        return;
    }
    glm_mat4_mulv3(skin, pos, 1.0f, out);
}

/*
 * Report triangle edges whose skinned length grew far beyond their bind
 * length. Rigid geometry stays at ratio ~1; skinning defects (vertices bound
 * to the wrong bone) show up as large ratios.
 */
static void report_skinning_stretch(SceneNode* node, const AnimationState* state) {
    if (!node)
        return;

    for (size_t m = 0; m < node->mesh_count; m++) {
        Mesh* mesh = node->meshes[m];
        if (!mesh || !mesh->is_skinned || !mesh->bone_ids || !mesh->indices ||
            mesh->draw_mode != TRIANGLES)
            continue;

        float worst_ratio = 0.0f;
        size_t worst_va = 0, worst_vb = 0;
        size_t stretched_edges = 0;

        for (size_t t = 0; t + 2 < mesh->index_count; t += 3) {
            for (int e = 0; e < 3; e++) {
                size_t va = mesh->indices[t + e];
                size_t vb = mesh->indices[t + (e + 1) % 3];
                if (va >= mesh->vertex_count || vb >= mesh->vertex_count)
                    continue;

                vec3 bind_a = {mesh->vertices[va * 3], mesh->vertices[va * 3 + 1],
                               mesh->vertices[va * 3 + 2]};
                vec3 bind_b = {mesh->vertices[vb * 3], mesh->vertices[vb * 3 + 1],
                               mesh->vertices[vb * 3 + 2]};
                float bind_len = glm_vec3_distance(bind_a, bind_b);
                if (bind_len < 1e-6f)
                    continue;

                vec3 skin_a = {0.0f, 0.0f, 0.0f};
                vec3 skin_b = {0.0f, 0.0f, 0.0f};
                cpu_skin_vertex(mesh, state, va, skin_a);
                cpu_skin_vertex(mesh, state, vb, skin_b);
                float skin_len = glm_vec3_distance(skin_a, skin_b);

                float ratio = skin_len / bind_len;
                if (ratio > 3.0f)
                    stretched_edges++;
                if (ratio > worst_ratio) {
                    worst_ratio = ratio;
                    worst_va = va;
                    worst_vb = vb;
                }
            }
        }

        if (stretched_edges > 0) {
            printf("STRETCH mesh[%zu] (%zu verts): %zu edges >3x, worst=%.1fx (v%zu <-> v%zu)\n",
                   m, mesh->vertex_count, stretched_edges, worst_ratio, worst_va, worst_vb);
            for (int side = 0; side < 2; side++) {
                size_t v = side == 0 ? worst_va : worst_vb;
                printf("  v%zu bones:", v);
                for (int i = 0; i < BONES_PER_VERTEX; i++) {
                    int id = mesh->bone_ids[v * BONES_PER_VERTEX + i];
                    float w = mesh->bone_weights[v * BONES_PER_VERTEX + i];
                    if (id >= 0 && w > 0.0f && mesh->skeleton &&
                        (size_t)id < mesh->skeleton->bone_count) {
                        printf(" '%s'=%.2f", mesh->skeleton->bones[id].name, w);
                    }
                }
                printf("\n");
            }
        } else {
            printf("STRETCH mesh[%zu] (%zu verts): ok, worst=%.2fx\n", m, mesh->vertex_count,
                   worst_ratio);
        }
    }

    for (size_t c = 0; c < node->children_count; c++) {
        report_skinning_stretch(node->children[c], state);
    }
}

/*
 * Callbacks
 */
void mouse_button_callback(Engine* engine, int button, int action, int mods) {
    if (drag_controller) {
        double x, y;
        glfwGetCursorPos(engine->window, &x, &y);
        mouse_drag_on_button(drag_controller, button, action, mods, x, y);
    }
}

void key_callback(Engine* engine, int key, int scancode, int action, int mods) {
    (void)scancode;

    // Camera movement (WASD, arrows, etc.)
    if (drag_controller && camera_controller_on_key(drag_controller, key, action, mods)) {
        return;
    }

    // App controls (only on press)
    if (action != GLFW_PRESS) {
        return;
    }

    switch (key) {
        case GLFW_KEY_ESCAPE:
            glfwSetWindowShouldClose(engine->window, GLFW_TRUE);
            break;
        case GLFW_KEY_G:
            set_engine_show_gui(engine, !engine->show_gui);
            break;
        case GLFW_KEY_X:
            set_engine_show_xyz(engine, !engine->show_xyz);
            break;
        case GLFW_KEY_T:
            set_engine_show_wireframe(engine, !engine->show_wireframe);
            break;
        case GLFW_KEY_1:
            engine->current_render_mode = RENDER_MODE_PBR;
            break;
        case GLFW_KEY_2:
            engine->current_render_mode = RENDER_MODE_NORMALS;
            break;
        case GLFW_KEY_3:
            engine->current_render_mode = RENDER_MODE_WORLD_POS;
            break;
        case GLFW_KEY_4:
            engine->current_render_mode = RENDER_MODE_TEX_COORDS;
            break;
        case GLFW_KEY_5:
            engine->current_render_mode = RENDER_MODE_TANGENT_SPACE;
            break;
        case GLFW_KEY_6:
            engine->current_render_mode = RENDER_MODE_FLAT_COLOR;
            break;
        default:
            break;
    }
}

void render_scene_callback(Engine* engine, Scene* current_scene) {
    SceneNode* root_node = current_scene->root_node;

    if (!engine || !root_node)
        return;

    if (max_frames > 0 && ++frames_rendered >= max_frames) {
        glfwSetWindowShouldClose(engine->window, GLFW_TRUE);
    }

    float time_value = glfwGetTime();
    float delta_time = time_value - last_frame_time;
    last_frame_time = time_value;

    // Snapshot last frame's pose for skinned motion vectors (TAA) before this
    // frame recomputes it. Every frame (even paused) so a still pose reads zero
    // deformation velocity instead of a frozen nonzero one that would smear it.
    animation_snapshot_prev_pose(anim_state);

    // Update animation
    if (anim_state && anim_state->playing) {
        update_animation(anim_state, delta_time);
        set_render_animation_state(anim_state);

        // One-shot stretch diagnostic once the animation is mid-pose
        if (check_stretch && frames_rendered == 60) {
            printf("\n===== SKINNING STRETCH CHECK (time=%.1f ticks) =====\n",
                   anim_state->current_time);
            report_skinning_stretch(root_node, anim_state);
            printf("===== END STRETCH CHECK =====\n\n");
        }
    }

    // Sync the camera zoom limit with the ground-projection fade start every
    // frame so GUI changes to Dome Radius take effect (enforcement lives in
    // camera_enforce_max_distance, applied by mouse_drag_update).
    if (engine->camera) {
        engine->camera->max_distance =
            (current_scene->render_skybox && current_scene->skybox_ground_projection)
                ? SKYBOX_GP_FADE_START * current_scene->skybox_gp_radius
                : 0.0f;
    }

    // Update camera via drag controller
    if (drag_controller) {
        mouse_drag_update(drag_controller, time_value);
    }

    // Distance-adaptive near plane: 0.02 x camera-to-target distance equals the
    // load-time near at the default 2.5x-radius framing (so nothing changes
    // until the user zooms), then shrinks with the camera so close-ups don't
    // clip into the model.
    if (engine->camera && clip_near_max > 0.0f) {
        float cam_dist = glm_vec3_distance(engine->camera->position, engine->camera->look_at);
        float near_clip = fmaxf(fminf(0.02f * cam_dist, clip_near_max), clip_near_floor);
        if (near_clip != engine->camera->near_clip) {
            engine->camera->near_clip = near_clip;
            update_engine_camera_perspective(engine);
        }
    }

    apply_model_recenter(engine, root_node);

    render_current_scene(engine, time_value);

    // Render skeleton bones if enabled
    if (engine->show_bones) {
        Skeleton* skel = (current_scene->skeleton_count > 0) ? current_scene->skeletons[0] : NULL;
        render_skeleton_bones(engine, skel, anim_state);
    }
}

/*
 * Configure iridescent visor material (pilot helmet style): a thin-film
 * coating over real transmissive glass. The film tints the specular
 * reflection (the rainbow sheen); transmission carries the refracted
 * scene through the shell. Opacity stays 1.0 -- transmission replaces
 * alpha as the see-through mechanism (mixing both composes wrongly).
 * filmThickness: coating thickness in nanometers (300-500nm for gold/rainbow effect)
 */
void set_node_iridescent_visor(SceneNode* node, float transmission, float roughness,
                               float filmThickness) {
    if (!node)
        return;

    for (size_t i = 0; i < node->mesh_count; i++) {
        Mesh* mesh = node->meshes[i];
        if (mesh && mesh->material) {
            mesh->material->opacity = 1.0f;
            mesh->material->transmission = transmission;
            mesh->material->roughness = roughness;
            mesh->material->metallic = 0.0f;
            mesh->material->ior = 1.5f;
            mesh->material->filmThickness = filmThickness;
        }
    }
}

/*
 * Configure visor materials for helmet models.
 */
void configure_visor_materials(Scene* scene) {
    if (!scene || !scene->root_node)
        return;

    // Common visor node names
    const char* visor_names[] = {"VISIERE_A", "VISIERE_B", "GLASSE", "visor", "Visor", NULL};

    for (int i = 0; visor_names[i] != NULL; i++) {
        SceneNode* node = find_node_by_name(scene->root_node, visor_names[i]);
        if (node) {
            printf("Configuring iridescent visor for: %s\n", visor_names[i]);
            // Refractive visor: high transmission, very glossy, 520nm iridescence
            set_node_iridescent_visor(node, 0.9f, 0.005f, 520.0f);
        }
    }
}

/*
 * Configure the SSS fixture's skin materials. glTF carries no subsurface, so tag
 * each named skin node with a distinct scatter profile (per-channel color +
 * world radius) in PostFX and mark its material as skin (subsurface strength 1),
 * recording the profile slot on the material so pbr_frag can flag its pixels.
 * The two fixture spheres scatter differently (warm+wide vs cool+tight) -- the
 * per-material-profile A/B. --sss-radius overrides every profile's radius,
 * --sss-color the first profile's color (both < 0 keep the per-material
 * defaults). The global --no-sss toggle gates the effect.
 */
void configure_sss_materials(Engine* engine, Scene* scene, float radius, const float* color) {
    if (!engine || !engine->postfx || !scene || !scene->root_node)
        return;
    // node name -> scatter profile (per-channel weight with red widest; world
    // radius). Defaults differ per material so the two spheres read distinctly.
    struct {
        const char* node;
        float color[3];
        float radius;
    } skins[] = {
        {"sss_skin_a", {1.0f, 0.35f, 0.25f}, 0.6f},  // warm skin, wide scatter
        {"sss_skin_b", {0.4f, 0.75f, 0.55f}, 0.15f}, // cool wax, tight scatter
    };
    postfx_reset_sss_profiles(engine->postfx);
    for (size_t k = 0; k < sizeof(skins) / sizeof(skins[0]); k++) {
        SceneNode* node = find_node_by_name(scene->root_node, skins[k].node);
        if (!node)
            continue;
        vec3 prof_color;
        glm_vec3_copy(skins[k].color, prof_color);
        if (k == 0 && color && color[0] >= 0.0f)
            glm_vec3_copy((float*)color, prof_color); // --sss-color overrides the first
        float prof_radius = radius >= 0.0f ? radius : skins[k].radius;
        int slot = postfx_add_sss_profile(engine->postfx, prof_color, prof_radius);
        for (size_t i = 0; i < node->mesh_count; i++) {
            Mesh* mesh = node->meshes[i];
            if (!mesh || !mesh->material)
                continue;
            Material* m = mesh->material;
            m->subsurface = 1.0f;          // mark as skin
            m->subsurface_profile = slot;  // pbr_frag writes this into the diffuse alpha
            glm_vec3_copy(prof_color, m->subsurface_color); // transmission tint follows the profile
        }
        printf("Configured SSS material %s (slot %d, radius %.3f, color %.2f,%.2f,%.2f)\n",
               skins[k].node, slot, prof_radius, prof_color[0], prof_color[1], prof_color[2]);
    }
}

/*
 * CETRA MAIN
 */
int main(int argc, char** argv) {
    RenderArgs args;

    if (parse_args(argc, argv, &args) != 0) {
        print_usage(argv[0]);
        return -1;
    }

    if (args.show_help) {
        print_usage(argv[0]);
        return 0;
    }

    // Two environment sources cannot coexist; silent precedence would hide
    // the mistake, so refuse outright
    if (args.sky && args.hdr_path) {
        fprintf(stderr, "Error: --sky and -e are mutually exclusive\n");
        return -1;
    }

    Engine* engine = create_engine("Cetra Engine", args.width, args.height);

    set_engine_headless(engine, args.headless != 0);
    engine->headless_jitter = args.headless_jitter != 0;
    set_engine_screenshot_path(engine, args.screenshot_path);
    set_engine_screenshot_every(engine, args.screenshot_every);
    if (args.ssaa > 0)
        set_engine_ss_scale(engine, args.ssaa);
    max_frames = args.max_frames;
    check_stretch = args.check_stretch;

    if (init_engine(engine) != 0) {
        fprintf(stderr, "Failed to initialize engine\n");
        return -1;
    }

    if (args.exposure > 0.0f && engine->postfx) {
        // A manual exposure pins the frame: auto-adaptation off
        engine->postfx->exposure = args.exposure;
        engine->postfx->auto_exposure = false;
    }
    if (args.no_auto_exposure && engine->postfx) {
        engine->postfx->auto_exposure = false;
    }
    if (args.no_ssao && engine->postfx) {
        engine->postfx->ssao_enabled = false;
    }
    if (args.no_spec_occlusion && engine->postfx) {
        engine->postfx->spec_occlusion_enabled = false;
    }
    if (args.no_ao_edge_filter && engine->postfx) {
        engine->postfx->ao_edge_filter_enabled = false;
    }
    if (args.ssao_debug && engine->postfx) {
        engine->postfx->debug_view = POSTFX_DEBUG_AO;
    }
    if (args.ssgi && engine->postfx) {
        engine->postfx->ssgi_enabled = true;
    }
    if (args.ssgi_debug && engine->postfx) {
        engine->postfx->debug_view = POSTFX_DEBUG_SSGI;
    }
    if (args.fog && engine->postfx) {
        engine->postfx->fog_enabled = true;
    }
    if (args.fog_debug && engine->postfx) {
        engine->postfx->debug_view = POSTFX_DEBUG_FOG;
    }
    if (args.albedo_debug && engine->postfx) {
        engine->postfx->debug_view = POSTFX_DEBUG_ALBEDO;
    }
    if (args.no_normals_mrt && engine->postfx) {
        engine->postfx->normals_enabled = false;
    }
    if (args.normals_debug && engine->postfx) {
        engine->postfx->debug_view = POSTFX_DEBUG_NORMALS;
    }
    if (args.no_ssr && engine->postfx) {
        engine->postfx->ssr_enabled = false;
    }
    if (args.no_ssr_full_res && engine->postfx) {
        postfx_set_ssr_full_res(engine->postfx, false);
    }
    if (args.no_ssr_temporal && engine->postfx) {
        engine->postfx->ssr_temporal = false;
    }
    if (args.no_ssr_denoise && engine->postfx) {
        engine->postfx->ssr_denoise = false;
    }
    if (args.ssr_jitter >= 0.0f && engine->postfx) {
        engine->postfx->ssr_jitter = args.ssr_jitter;
    }
    if (args.ssr_debug && engine->postfx) {
        engine->postfx->debug_view = POSTFX_DEBUG_SSR;
    }
    if (args.ssr_strength >= 0.0f && engine->postfx) {
        engine->postfx->ssr_strength = args.ssr_strength;
    }
    if (args.specular_aa >= 0.0f) {
        engine->specular_aa_strength = args.specular_aa;
    }
    if (args.no_energy_comp) {
        engine->energy_comp_enabled = false;
    } else if (!args.hdr_path) {
        // Default-on but needs the IBL BRDF LUT, which only exists with an
        // environment -- say so instead of leaving a silently inert toggle
        fprintf(stderr, "Note: energy compensation is inactive without an HDR environment (-e)\n");
    }
    if (args.no_refraction) {
        engine->refraction_enabled = false;
    }
    if (args.no_clearcoat) {
        engine->clearcoat_enabled = false;
    }
    if (args.no_specular) {
        engine->specular_enabled = false;
    }
    if (args.no_sheen) {
        engine->sheen_enabled = false;
    }
    if (args.no_parallax) {
        engine->parallax_enabled = false;
    }
    if (args.no_sss) {
        engine->sss_enabled = false;
    }
    // Set the POM default depth before the model loads (the height convention
    // loader stamps it onto materials as it resolves their height maps).
    if (args.parallax_scale >= 0.0f) {
        set_parallax_default_scale(args.parallax_scale);
    }
    if (engine->postfx) {
        PostFX* fx = engine->postfx;
        if (args.no_bloom) {
            fx->bloom_enabled = false;
        }
        if (args.tonemap_mode != 0) {
            fx->tonemap_mode = (PostFXTonemapMode)args.tonemap_mode;
        }
        // --film applies the whole finishing look first, so individual
        // finishing flags below can still override any part of it.
        if (args.film_preset) {
            postfx_apply_film_look(fx);
        }
        if (args.no_vignette) {
            fx->vignette_enabled = false;
        }
        if (args.vignette >= 0.0f) {
            fx->vignette_enabled = true;
            fx->vignette_strength = args.vignette;
        }
        if (args.grain >= 0.0f) {
            fx->grain_enabled = true;
            fx->grain_strength = args.grain;
        }
        if (args.sharpen >= 0.0f) {
            fx->sharpen_enabled = true;
            fx->sharpen_strength = args.sharpen;
        }
        if (args.grade_set) {
            fx->grade_enabled = true;
            glm_vec3_copy(args.grade_lift, fx->grade_lift);
            glm_vec3_copy(args.grade_gamma, fx->grade_gamma);
            glm_vec3_copy(args.grade_gain, fx->grade_gain);
        }
    }
    if (args.render_mode > 0) {
        engine->current_render_mode = (RenderMode)args.render_mode;
    }

    set_engine_error_callback(engine, app_error_callback);
    set_engine_mouse_button_callback(engine, mouse_button_callback);
    set_engine_key_callback(engine, key_callback);

    /*
     * Set up shaders.
     *
     */
    ShaderProgram* pbr_shader_program = get_engine_shader_program_by_name(engine, "pbr");
    if (!pbr_shader_program) {
        fprintf(stderr, "Failed to get PBR shader program\n");
        return -1;
    }

    // Create skinned PBR shader for skeletal animation
    ShaderProgram* pbr_skinned_program = create_pbr_skinned_program();
    if (!pbr_skinned_program) {
        fprintf(stderr, "Failed to create PBR skinned shader program\n");
        return -1;
    }
    add_shader_program_to_engine(engine, pbr_skinned_program);

    ShaderProgram* xyz_shader_program = get_engine_shader_program_by_name(engine, "xyz");
    if (!xyz_shader_program) {
        fprintf(stderr, "Failed to get xyz shader program\n");
        return -1;
    }

    /*
     * Set up camera.
     */
    vec3 camera_position = {0.0f, 150.0f, 100.0f};
    vec3 look_at_point = {0.0f, 150.0f, 0.0f};
    vec3 up_vector = {0.0f, 1.0f, 0.0f};
    float fov_radians = glm_rad(args.fov_deg > 0.0f ? args.fov_deg : 50.0f);
    float near_clip = 7.0f;
    float far_clip = 10000.0f;

    Camera* camera = create_camera();

    set_camera_position(camera, camera_position);
    set_camera_look_at(camera, look_at_point);
    set_camera_up_vector(camera, up_vector);
    set_camera_perspective(camera, fov_radians, near_clip, far_clip);

    update_engine_camera_lookat(engine);
    update_engine_camera_perspective(engine);

    camera->theta = 0.60f;
    camera->height = 600.0f;

    set_engine_camera(engine, camera);

    // Create drag controller with auto-orbit (fixed camera in headless mode for
    // deterministic, comparable screenshots)
    drag_controller = create_mouse_drag_controller(engine);
    set_mouse_drag_auto_orbit(drag_controller, !args.headless, CAM_ANGULAR_SPEED, MIN_DIST,
                              MAX_DIST);

    /*
     * Import model with async texture loading.
     */

    if (args.no_flip_uv)
        set_import_flip_uvs(false);
    // The importer defaults the texture dir to the model's own directory when
    // none is given (so an external-texture glTF like the POM fixture loads
    // without -t), so just pass args.texture_dir through.
    Scene* scene =
        create_scene_from_model_path_async(args.model_path, args.texture_dir, engine->async_loader);
    if (!scene) {
        fprintf(stderr, "Failed to import model: %s\n", args.model_path);
        return -1;
    }

    add_scene_to_engine(engine, scene);

    if (!scene || !scene->root_node) {
        fprintf(stderr, "Failed to import scene\n");
        return -1;
    }

    if (set_scene_xyz_shader_program(scene, xyz_shader_program) == GL_FALSE) {
        fprintf(stderr, "Failed to set scene xyz shader program\n");
        return -1;
    }

    configure_visor_materials(scene);
    configure_sss_materials(engine, scene, args.sss_radius, args.sss_color);

    if (args.sky) {
        // Procedural sky: bake the atmosphere LUTs + sky-view + environment
        // cubemap into a fresh IBLResources so the entire IBL/skybox/probe/
        // fog pipeline follows the sun exactly like an -e HDR would. The sun
        // becomes the single extracted "lobe" the key-light rig below
        // consumes.
        SkyAtmosphere* sky = create_sky_atmosphere();
        IBLResources* ibl = create_ibl_resources();
        if (sky) {
            // CLI sun placement must land before the bake (the LUTs depend
            // on the sun position)
            if (args.sun_elevation > -900.0f)
                sky->sun_elevation_deg = args.sun_elevation;
            if (args.sun_azimuth > -900.0f)
                sky->sun_azimuth_deg = args.sun_azimuth;
            sky_update_sun_dir(sky);
        }
        if (sky && ibl && sky_bake_static_luts(sky, engine) == 0 &&
            sky_bake(sky, ibl, engine) == 0) {
            sky->debug_luts = args.sky_debug != 0;
            scene->sky = sky;
            scene->ibl = ibl;
            scene->render_skybox = true;
            scene->skybox_brightness = 1.0f;
            // No photographic floor to project; the sky's virtual ground
            // grounds the model instead
            scene->skybox_ground_projection = false;
            // Create the sun as a shadow-casting directional key light and
            // couple it to the atmosphere: the sky module owns its direction,
            // transmittance tint (warm near the horizon), and below-horizon
            // fade, so a moving sun drives the light, shadows, and fog through
            // one path. --no-key-light leaves a pure-IBL sky. The IBL lobe
            // toward the sun is set inside sky_bake.
            if (!args.no_key_light) {
                Light* sun = create_light();
                if (sun) {
                    set_light_name(sun, "sky_sun");
                    set_light_type(sun, LIGHT_DIRECTIONAL);
                    sky->sun_light = sun;
                    sky->sun_base_intensity = KEY_LIGHT_TOTAL_INTENSITY;
                    sky_apply_sun_to_light(sky);
                    add_light_to_scene(scene, sun);
                    SceneNode* sun_node = create_node();
                    set_node_light(sun_node, sun);
                    set_node_name(sun_node, "sky_sun");
                    add_child_node(scene->root_node, sun_node);
                }
                // Ground the model on the virtual floor
                scene->shadow_catcher = true;
            }
            printf("Procedural sky: sun at elevation %.1f azimuth %.1f\n",
                   sky->sun_elevation_deg, sky->sun_azimuth_deg);

            // Diagnostic soak (the M4 leak gate): drive the dynamic-sun
            // re-bake path (the GUI's sky_update_sun) N times across the whole
            // elevation/azimuth range -- including below-horizon, where the
            // key light fades and the sky-view LUT hits its ground branch --
            // then restore the requested sun. GL errors along the way flag a
            // resource leak or state corruption; a clean run proves the
            // re-bake is safe to fire per-frame during a GUI drag.
            if (args.sky_rebake_stress > 0) {
                float el0 = sky->sun_elevation_deg, az0 = sky->sun_azimuth_deg;
                int gl_errors = 0;
                for (int i = 0; i < args.sky_rebake_stress; i++) {
                    sky->sun_elevation_deg = -6.0f + (float)(i % 96);
                    sky->sun_azimuth_deg = (float)((i * 37) % 360);
                    sky_update_sun(sky, ibl, engine);
                    if (glGetError() != GL_NO_ERROR)
                        gl_errors++;
                }
                sky->sun_elevation_deg = el0;
                sky->sun_azimuth_deg = az0;
                sky_update_sun(sky, ibl, engine);
                printf("Sky rebake stress: %d cycles complete, %d GL errors\n",
                       args.sky_rebake_stress, gl_errors);
            }
        } else {
            fprintf(stderr, "Failed to bake procedural sky\n");
            if (sky)
                free_sky_atmosphere(sky);
            if (ibl)
                free_ibl_resources(ibl);
        }
    }

    if (args.hdr_path) {
        IBLResources* ibl = create_ibl_resources();
        if (ibl && load_hdr_environment(ibl, args.hdr_path) == 0) {
            if (precompute_ibl(ibl, engine) == 0) {
                scene->ibl = ibl;
                scene->render_skybox = true;
                scene->skybox_brightness = 1.0f;
                // Ground projection on by default; --no-ground restores the
                // infinite skybox. Dome radius/height are set with the other
                // scene-scaled policies once the bounds are known.
                scene->skybox_ground_projection = !args.no_ground;
                printf("IBL loaded from: %s\n", args.hdr_path);
            } else {
                fprintf(stderr, "Failed to precompute IBL\n");
                free_ibl_resources(ibl);
                ibl = NULL;
            }
        } else {
            fprintf(stderr, "Failed to load HDR: %s\n", args.hdr_path);
            if (ibl)
                free_ibl_resources(ibl);
            ibl = NULL;
        }

        // Add one soft shadow-casting key light per bright lobe extracted
        // from the HDR, so each visible studio light casts its own shadow.
        // Total analytic intensity stays ~1.0, split by lobe energy.
        // --no-key-light gives pure image-based lighting instead.
        if (!args.no_key_light) {
            int lobes = (scene->ibl && scene->ibl->light_count > 0) ? scene->ibl->light_count : 1;
            for (int i = 0; i < lobes; i++) {
                Light* key = create_light();
                if (!key)
                    continue;

                char light_name[32];
                snprintf(light_name, sizeof(light_name), "key_light_%d", i);
                set_light_name(key, light_name);
                set_light_type(key, LIGHT_DIRECTIONAL);

                vec3 key_dir = {-0.4f, -0.7f, -0.6f}; // Fallback if no lobes
                float intensity = KEY_LIGHT_TOTAL_INTENSITY;
                if (scene->ibl && scene->ibl->light_count > 0) {
                    glm_vec3_negate_to(scene->ibl->light_dirs[i], key_dir);
                    // Blend the HDR energy split toward an even split so weak
                    // fill lobes still light their side (photographic fill
                    // ratio) instead of leaving it to the rim light alone
                    float share = 0.5f * scene->ibl->light_energies[i] + 0.5f / (float)lobes;
                    intensity = share * KEY_LIGHT_TOTAL_INTENSITY;
                }
                set_light_direction(key, key_dir);
                set_light_intensity(key, intensity);
                set_light_color(key, (vec3){1.0f, 1.0f, 1.0f});
                set_light_cast_shadows(key, true);
                add_light_to_scene(scene, key);

                SceneNode* key_node = create_node();
                set_node_light(key_node, key);
                set_node_name(key_node, light_name);
                add_child_node(scene->root_node, key_node);
            }

            // Ground the model: catch the key light shadows on the
            // projected environment floor
            scene->shadow_catcher = scene->ibl != NULL;
        }
    } else if (scene->light_count == 0) {
        // No IBL and the asset brings no lights of its own: add a neutral
        // three-point rig so a bare model is visible. Scenes that ship their
        // own lighting design (embedded lights, emissive surfaces) keep it --
        // flooding them with a default rig erases the authored mood.
        create_three_point_lights(scene, 3.0f);
    }

    // Load additional animation files if provided
    // Enable retargeting by default to support Mixamo animations on custom rigs
    size_t first_cli_anim = 0; // Index of first animation loaded via -a (0 = embedded)
    if (args.anim_count > 0 && scene->skeleton_count > 0) {
        first_cli_anim = scene->animation_count;
        Skeleton* skeleton = scene->skeletons[0];

        // Load source skeleton for retargeting if provided
        Skeleton* source_skeleton = NULL;
        if (args.source_skeleton_path) {
            Scene* source_scene = create_scene_from_model_path(args.source_skeleton_path, NULL);
            if (source_scene && source_scene->skeleton_count > 0) {
                source_skeleton = source_scene->skeletons[0];
                printf("Loaded source skeleton: %zu bones from '%s'\n",
                       source_skeleton->bone_count, args.source_skeleton_path);
            } else {
                fprintf(stderr, "Warning: failed to load source skeleton '%s'\n",
                        args.source_skeleton_path);
            }
        }

        for (int i = 0; i < args.anim_count; i++) {
            int loaded = load_animations_from_file(scene, skeleton, args.anim_files[i],
                                                   true, source_skeleton);
            if (loaded < 0) {
                fprintf(stderr, "Warning: failed to load animation '%s'\n", args.anim_files[i]);
            }
        }
        printf("Total animations: %zu\n", scene->animation_count);
    } else if (args.anim_count > 0) {
        fprintf(stderr, "Warning: animation files specified but model has no skeleton\n");
    }

    // Start playing an animation if available; prefer the first one loaded via -a
    // over animations embedded in the model file
    if (scene->animation_count > 0 && scene->skeleton_count > 0) {
        size_t play_idx = (first_cli_anim < scene->animation_count) ? first_cli_anim : 0;
        anim_state = create_animation_state(scene->skeletons[0]);
        if (anim_state) {
            set_animation(anim_state, scene->animations[play_idx]);
            anim_state->looping = true;
            play_animation(anim_state);
            printf("Playing animation: %s (index %zu of %zu)\n", scene->animations[play_idx]->name,
                   play_idx, scene->animation_count);

            // Spring-bone secondary motion for chains no animation drives.
            // Only hair: the "sheath root" chain is a rigid metal belt on
            // this rig and must stay welded to its authored pose.
            anim_state->springs = create_spring_bone_system(scene->skeletons[0]);
            if (anim_state->springs) {
                int n = spring_bone_add_chains_by_prefix(anim_state->springs, "hair");
                if (n > 0) {
                    printf("Spring bones: %d chain(s) under prefix 'hair'\n", n);
                }
                if (args.no_springs) {
                    anim_state->springs->enabled = false;
                }
            }
        }
    }

    upload_buffers_to_gpu_for_nodes(scene->root_node);

    set_shader_programs_for_nodes(scene->root_node, pbr_shader_program, pbr_skinned_program);

    // Propagate transforms before computing bounds (needed for correct global_transform values)
    mat4 identity;
    glm_mat4_identity(identity);
    apply_transform_to_nodes(scene->root_node, identity);

    // Compute scene bounds; center/radius drive every scene-scaled policy below
    vec3 scene_center;
    float scene_radius;
    compute_scene_center_and_radius(scene, scene_center, &scene_radius);

    // Recenter the model: translate so the bounding-box base sits on the origin
    // (y=0, centered in x/z). Everything anchored at the origin -- the
    // ground-projection dome, the shadow catcher, the orbit pivot -- assumes the
    // model stands there; off-origin assets (e.g. authored floating at y=189)
    // otherwise streak the projected environment and z-fight. The offset rides
    // the per-frame model matrix (so animation-driven node transforms compose
    // under it untouched); a pure translation leaves the radius unchanged, so
    // the cached center just shifts by the offset.
    vec3 bb_min, bb_max;
    compute_scene_bounds(scene, bb_min, bb_max);
    if (!args.no_recenter) {
        model_recenter_offset[0] = -scene_center[0];
        model_recenter_offset[1] = -bb_min[1];
        model_recenter_offset[2] = -scene_center[2];
        if (glm_vec3_norm(model_recenter_offset) > 1e-4f) {
            printf("Recentering model by (%.2f, %.2f, %.2f)\n", model_recenter_offset[0],
                   model_recenter_offset[1], model_recenter_offset[2]);
        }
        glm_vec3_add(scene_center, model_recenter_offset, scene_center);
    }
    // World height of the ground plane wherever the model ended up (0 when
    // recentered, the authored base otherwise) -- the fog density anchors
    // to it, and deriving it from the offset keeps it correct under any
    // future recenter rule
    float scene_floor_y = bb_min[1] + model_recenter_offset[1];
    printf("Scene bounds: center=(%.2f, %.2f, %.2f), radius=%.2f\n", scene_center[0],
           scene_center[1], scene_center[2], scene_radius);

    // Size the ground-projection dome to the scene, not a fixed 5 units. The
    // camera's zoom cap derives from the dome radius (max_distance = fade_start
    // * dome_radius), so a fixed dome clamped off-scale assets (e.g. a 773-unit
    // scene) to a few units from center -- huge framing and dead zoom. Scaling
    // it keeps framing and zoom range proportional at any model scale. An
    // explicit --ground still wins. 5x radius leaves headroom past the 2.5x
    // auto-framing distance (cap = 0.7 * 5 = 3.5x radius).
    if (scene->render_skybox && scene->skybox_ground_projection) {
        // The single policy site for the dome geometry: an explicit arg wins,
        // else scale to the scene, else keep the human-scale defaults. The
        // height tracks the radius at the defaults' 1.2/5 ratio -- a fixed
        // human-scale capture height against a scene-scaled dome squashes the
        // projection geometry and smears the panorama's floor into radial
        // streaks around large models.
        scene->skybox_gp_radius = args.ground_radius > 0.0f ? args.ground_radius
                                  : scene_radius > 0.0f    ? scene_radius * 5.0f
                                                           : 5.0f;
        scene->skybox_gp_height = args.ground_height > 0.0f
                                      ? args.ground_height
                                      : scene->skybox_gp_radius * (1.2f / 5.0f);
    }

    // Fit the shadow frustum to the scene; the library default (ortho 2000)
    // leaves a human-sized model with no effective shadow map resolution
    if (scene->shadow_system && args.no_shadows) {
        scene->shadow_system->enabled = false;
    }
    if (scene->shadow_system && scene_radius > 0.0f) {
        scene->shadow_system->ortho_size = scene_radius * 2.0f;
        scene->shadow_system->near_plane = 0.1f;
        scene->shadow_system->far_plane = scene_radius * 8.0f;

        // Contact-hardening shadows on by default in the viewer. The library
        // default light size (50m) is absurd against a ~2x-scene-radius ortho
        // frustum, so size the emitter to the scene; --light-size overrides.
        scene->shadow_system->pcss_enabled = !args.no_pcss;
        if (args.shadow_softness >= 0.0f) {
            scene->shadow_system->pcss_softness = args.shadow_softness;
        }
        // Cascaded maps on by default in the viewer (the library default
        // stays 1): near shadows at high effective resolution, snap-stable
        // under orbit. --shadow-cascades 1 restores the classic single map.
        scene->shadow_system->cascade_count =
            args.shadow_cascades > 0 ? args.shadow_cascades : SHADOW_CASCADES;
        if (args.csm_debug) {
            scene->shadow_system->csm_debug = true;
        }
        float light_size = args.light_size >= 0.0f ? args.light_size : scene_radius * 0.08f;
        for (size_t i = 0; i < scene->light_count; i++) {
            Light* light = scene->lights[i];
            if (light && light->type == LIGHT_DIRECTIONAL && light->cast_shadows) {
                set_light_size(light, light_size, light_size);
            }
        }
    }

    // Position camera to view the entire scene
    float camera_distance = scene_radius * 2.5f;
    if (camera_distance < 1.0f)
        camera_distance = 100.0f; // Fallback for empty scenes
    if (args.camera_distance > 0.0f)
        camera_distance = args.camera_distance;

    float yaw = glm_rad(args.orbit_yaw);
    float pitch = glm_rad(args.orbit_pitch);
    vec3 auto_cam_pos = {scene_center[0] + camera_distance * cosf(pitch) * sinf(yaw),
                         scene_center[1] + scene_radius * 0.3f + camera_distance * sinf(pitch),
                         scene_center[2] + camera_distance * cosf(pitch) * cosf(yaw)};
    set_camera_position(camera, auto_cam_pos);
    set_camera_look_at(camera, scene_center);

    // Depth of field focuses on the subject (camera-to-model distance) unless
    // overridden. --film turns it on too; --no-dof forces it off. Range scales
    // with the scene so the model stays sharp and the backdrop falls away.
    if ((args.dof || args.film_preset) && !args.no_dof && engine->postfx) {
        PostFX* fx = engine->postfx;
        fx->dof_enabled = true;
        if (args.dof_focus > 0.0f) {
            // A pinned manual focus plane turns autofocus off
            fx->dof_focus_distance = args.dof_focus;
            fx->dof_autofocus = false;
        } else {
            // Autofocus (the default) tracks the subject each frame; seed it
            // with the initial camera-to-model distance for frame zero
            fx->dof_focus_distance = glm_vec3_distance(auto_cam_pos, scene_center);
            fx->dof_autofocus = true;
        }
        fx->dof_focus_range = args.dof_range > 0.0f ? args.dof_range : scene_radius * 1.5f;
        if (args.dof_max_coc > 0.0f)
            fx->dof_max_coc = args.dof_max_coc;
    }

    // Motion blur: off unless requested. Velocity comes from the aux buffer
    // (already produced for TAA/SSAO), so this is a toggle plus an optional
    // shutter scale.
    if (args.motion_blur && engine->postfx) {
        engine->postfx->motion_blur_enabled = true;
        if (args.motion_blur_scale >= 0.0f)
            engine->postfx->motion_blur_scale = args.motion_blur_scale;
    }

    // Clip planes tuned for depth precision, not just coverage. A huge
    // near/far ratio wrecks the 24-bit depth buffer, and cross-rig
    // retargeting pushes layered armor plates near-coplanar — with bad
    // precision they z-fight into per-pixel speckle along every seam that
    // shifts with each pose. Keep the range tight around the scene (the old
    // "far < 100 -> 10000" fallback gave a ~700000:1 ratio on human-scale
    // models). near is the dominant precision term, so lift it off zero.
    float auto_near = fmaxf(scene_radius * 0.05f, 0.05f);
    float auto_far = scene_radius * 40.0f;
    set_camera_perspective(camera, fov_radians, auto_near, auto_far);
    update_engine_camera_perspective(engine);
    printf("Camera clip planes: near=%.4f, far=%.2f\n", auto_near, auto_far);

    // Arm the per-frame distance-adaptive near (see the render callback). The
    // floor matches the orbit's 10% minimum zoom (0.02 * 0.25 * camera_distance).
    clip_near_max = auto_near;
    clip_near_floor = fmaxf(auto_near * 0.1f, 0.005f);

    // AO/GI reach scales with the scene like the shadow frustum and the dome:
    // the meter-scale default (0.4) is sub-resolution on large scenes -- the
    // whole effect collapses below one depth texel and gates itself off. 1% of
    // the bounding radius keeps the reach local (contact occlusion, room-scale
    // bounce); much larger and the sparse 8-step march ring-aliases distant
    // geometry into concentric bands. Small scenes keep the tuned default;
    // --ao-radius pins an explicit reach.
    if (engine->postfx) {
        if (args.ao_radius > 0.0f) {
            engine->postfx->ssao_radius = args.ao_radius;
        } else if (scene_radius > 0.0f) {
            engine->postfx->ssao_radius =
                fmaxf(engine->postfx->ssao_radius, scene_radius * 0.01f);
        }

        // The SSR march reach and thickness are world-space distances like
        // the AO radius: the meter-scale defaults leave a large-unit scene
        // with a march that spans a fraction of the model and a thickness
        // below the depth quantization at its distances. Scale both up with
        // the scene (never down: small scenes keep the tuned defaults).
        if (scene_radius > 0.0f) {
            engine->postfx->ssr_max_distance =
                fmaxf(engine->postfx->ssr_max_distance, scene_radius * 2.0f);
            engine->postfx->ssr_thickness =
                fmaxf(engine->postfx->ssr_thickness, scene_radius * 0.002f);

            // Fog parameters are world-space too: fixed meter-scale density
            // on a large-unit scene is invisible (or opaque soup on a tiny
            // one). Density targets ~18% extinction over the default
            // camera-to-subject path at ground level — present, not
            // smothering; the falloff spans half the model height; sky rays
            // march the whole dome interior.
            engine->postfx->fog_density =
                args.fog_density > 0.0f ? args.fog_density : 0.08f / scene_radius;
            engine->postfx->fog_height_falloff =
                args.fog_height > 0.0f ? args.fog_height : scene_radius * 0.5f;
            engine->postfx->fog_far = scene_radius * 5.0f;
            engine->postfx->fog_floor_y = scene_floor_y;
        }
    }

    // Update orbit controller with appropriate distance
    camera->distance = camera_distance;
    camera->height = scene_center[1];
    float orbit_max = camera_distance * 2.0f;
    if (scene->skybox_ground_projection) {
        // Keep the camera where the ground projection renders at full
        // strength; no reachable view ever shows the blend toward the
        // infinite skybox. Raising Dome Radius extends the zoom range.
        camera->max_distance = SKYBOX_GP_FADE_START * scene->skybox_gp_radius;
        orbit_max = fminf(orbit_max, camera->max_distance);
    }
    set_mouse_drag_auto_orbit(drag_controller, !args.headless, CAM_ANGULAR_SPEED,
                              fminf(camera_distance * 0.5f, orbit_max), orbit_max);

    update_engine_camera_lookat(engine);

    // Explicit camera pose override (--cam-eye/--cam-target): reproduce any
    // interactive view exactly, bypassing the yaw/pitch/distance orbit framing
    // above (which always looks at scene_center and cannot express a panned
    // view). Reuses the scene-derived clip planes and orbit limits set above;
    // the per-frame distance-adaptive near tracks the new cam-to-target span.
    if (args.cam_eye_set && args.cam_target_set) {
        vec3 up = {0.0f, 1.0f, 0.0f};
        if (args.cam_up_set)
            glm_vec3_copy(args.cam_up, up);
        set_camera_position(camera, args.cam_eye);
        set_camera_look_at(camera, args.cam_target);
        set_camera_up_vector(camera, up);
        camera->distance = glm_vec3_distance(args.cam_eye, args.cam_target);
        update_engine_camera_lookat(engine);
    } else if (args.cam_eye_set || args.cam_target_set) {
        fprintf(stderr,
                "Warning: --cam-eye and --cam-target must both be given; ignoring camera pose.\n");
    }

    print_scene(scene);

    // No GUI/FPS overlay in headless runs: the FPS digits change per run and
    // land in screenshots, which breaks byte-comparability
    set_engine_show_gui(engine, !args.headless);
    set_engine_show_fps(engine, !args.headless);
    set_engine_show_wireframe(engine, false);
    set_engine_show_xyz(engine, false);
    engine->show_bones = args.show_bones != 0;

    // Capture the local reflection probe: the scene rendered once into a
    // prefiltered cubemap, consumed as parallax-corrected local specular and
    // as the SSR miss fallback. Runs after every scene-scaled policy and the
    // overlay flags above are final (debug overlays must not bake into the
    // capture), but before the render loop starts.
    if (args.probe && scene->ibl && scene->ibl->precomputed) {
        // The capture renders node globals directly, so bake in the recenter
        // transform the per-frame callback applies before every draw (the
        // callback re-applies the same transform; no residue)
        apply_model_recenter(engine, scene->root_node);

        // A dome stage grounds the environment itself (no scene render); the
        // scene meshes join the capture only for interiors, where they ARE
        // the environment
        bool probe_env_only =
            !args.probe_scene && scene->render_skybox && scene->skybox_ground_projection;

        ReflectionProbe* probe = create_reflection_probe();
        if (probe) {
            // Fresh bounds: the pre-recenter ones above no longer describe
            // where the model sits
            vec3 probe_bb_min, probe_bb_max;
            compute_scene_bounds(scene, probe_bb_min, probe_bb_max);

            if (args.probe_pos_set) {
                glm_vec3_copy(args.probe_pos, probe->position);
            } else if (probe_env_only) {
                // The parallax origin of a grounded environment must be the
                // environment's own capture point: the box wall base then
                // lines up with the env's floor/wall junction, so up-going
                // reflection rays never map into the env's below-horizon
                // floor content (which would tile the floor's own image
                // into reflections as stripe rows).
                probe->position[0] = 0.0f;
                probe->position[1] = scene->skybox_gp_height;
                probe->position[2] = 0.0f;
            } else {
                // Scene capture: the scene center sits INSIDE a solid model,
                // where the capture photographs interior faces at point-blank
                // range and reflections tile that garbage everywhere. Offset
                // toward the initial camera into the empty space a viewer
                // occupies — still well inside a room-scale interior.
                vec3 to_cam;
                glm_vec3_sub(auto_cam_pos, scene_center, to_cam);
                glm_vec3_normalize(to_cam);
                glm_vec3_scale(to_cam, 0.6f * scene_radius, to_cam);
                glm_vec3_add(scene_center, to_cam, probe->position);
            }

            // Proxy box: bottom locked to the floor plane so floor-adjacent
            // reflections parallax-correct exactly. The walls/ceiling must
            // sit where the captured content actually is: at the dome for a
            // grounded environment (a scene-sized ceiling would warp its
            // reflection into diagonal streaks sliding across the floor), at
            // the scene bounds when the meshes themselves are captured
            // (interior walls).
            float box_half_w, box_top;
            if (probe_env_only) {
                box_half_w = scene->skybox_gp_radius;
                box_top = scene->skybox_gp_radius;
            } else {
                box_half_w = 2.0f * scene_radius;
                box_top = probe_bb_max[1] + scene_radius;
            }
            probe->box_min[0] = scene_center[0] - box_half_w;
            probe->box_min[1] = probe_bb_min[1];
            probe->box_min[2] = scene_center[2] - box_half_w;
            probe->box_max[0] = scene_center[0] + box_half_w;
            probe->box_max[1] = box_top;
            probe->box_max[2] = scene_center[2] + box_half_w;

            probe->debug_background = args.probe_debug != 0;

            // Capture frustum: scene-scaled near, far past the dome so the
            // projected environment lands in the capture
            float probe_near = fmaxf(0.005f * scene_radius, 0.01f);
            float probe_far = (scene->render_skybox && scene->skybox_ground_projection)
                                  ? 2.0f * scene->skybox_gp_radius
                                  : 10.0f * fmaxf(scene_radius, 1.0f);
            // Attach only after a successful capture: consumers treat an
            // attached probe as ready, and the capture pass itself must
            // never see one
            if (reflection_probe_capture(probe, engine, scene, probe_near, probe_far,
                                         probe_env_only) == 0) {
                scene->probe = probe;
            } else {
                free_reflection_probe(probe);
            }
        }
    } else if (args.probe) {
        fprintf(stderr, "Warning: --probe requires an HDR environment (-e); skipping capture\n");
    }

    // Interactive default: TAA-only (drop to 1x MSAA and let temporal AA carry
    // it) — much cheaper than 4x MSAA on this GPU and better on shading/specular
    // aliasing. Headless keeps 4x MSAA with TAA off so screenshots stay
    // deterministic (jitter + history accumulation would vary run to run).
    // --taa additionally exercises the temporal passes (TAA/AO/GI
    // accumulation) headless as a diagnostic: jitter + history make output
    // run-to-run sensitive to async load timing, so it is not for
    // byte-compared screenshots.
    if (!args.headless || args.force_taa) {
        set_engine_msaa_samples(engine, 1);
        set_engine_taa_enabled(engine, true);
    }

    run_engine_render_loop(engine, render_scene_callback);

    printf("Cleaning up...\n");
    if (anim_state) {
        free_animation_state(anim_state);
    }
    free_mouse_drag_controller(drag_controller);
    free_engine(engine);

    printf("Goodbye Friend...\n");

    return 0;
}
