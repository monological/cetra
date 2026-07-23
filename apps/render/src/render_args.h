#ifndef RENDER_ARGS_H
#define RENDER_ARGS_H

/*
 * Command line arguments / merged scene-file values. CLI flags win; scene
 * files (.cscn) fill fields the CLI left at their sentinels (see
 * cscene_apply.c); the application blocks in main read only this struct.
 */

#define MAX_ANIM_FILES 32

typedef struct {
    const char* model_path;
    const char* texture_dir;
    const char* hdr_path;
    const char* anim_files[MAX_ANIM_FILES];
    int anim_count;
    const char* source_skeleton_path; // Source skeleton for retargeting
    const char* screenshot_path;      // Save final frame here (PPM)
    int screenshot_every;             // Also save numbered frames every N frames
    float fov_deg;                    // Camera FOV in degrees (0 = default 50)
    float exposure;                   // Tonemap exposure override (0 = engine default)
    float ground_radius;              // Skybox ground projection dome radius (0 = default)
    float ground_height;              // HDR capture height above ground (0 = default)
    float camera_distance;            // Camera distance override in meters (0 = auto)
    int no_recenter;                  // Keep the model's authored world position
    int no_auto_exposure;             // Fixed exposure instead of eye adaptation
    int no_flip_uv;                   // For assets baked with the opposite V convention
    float ao_radius;                  // AO/GI reach override in world units (0 = auto)
    int force_taa;                    // TAA even in headless (temporal passes active)
    int no_ground;                    // Disable skybox ground projection
    int no_key_light;                 // Pure IBL: skip the analytic key lights
    int no_shadows;                   // Keep key lights but disable shadow maps
    int no_pcss;                      // Fixed-width PCF instead of contact-hardening
    float light_size;                 // Emitter size override (-1 = scene default)
    float shadow_softness;            // PCSS softness override (-1 = default)
    int shadow_cascades;              // Cascades per caster (0 = keep engine default)
    int csm_debug;                    // Tint fragments by selected cascade
    int no_springs;                   // Disable spring-bone secondary motion
    int no_ssao;                      // Disable screen-space ambient occlusion
    int ssao_debug;                   // Show the raw SSAO buffer
    int no_spec_occlusion;            // Let GTAO darken specular (disable spec-occ)
    int no_ao_edge_filter;            // Disable the depth-aware AO blur (allow silhouette bleed)
    int ssgi;                         // Enable screen-space GI (indirect diffuse)
    int ssgi_debug;                   // Show the raw gathered GI radiance
    int probe;                        // Enable the local reflection probe
    int probe_pos_set;                // --probe-pos given
    float probe_pos[3];               // Probe capture position override
    int probe_scene;                  // Capture the scene meshes too (interiors)
    int probe_debug;                  // Show the raw capture as the background
    int sky;                          // Procedural physically-based sky instead of -e
    int sky_debug;                    // Blit the sky LUTs into the frame corner
    float sun_elevation;              // Sky sun elevation in degrees (-999 = default)
    float sun_azimuth;                // Sky sun azimuth in degrees (-999 = default)
    int sky_rebake_stress;            // Diagnostic: N headless sun re-bakes then restore
    int fog;                          // Enable volumetric fog
    int fog_debug;                    // Show the raw fog in-scatter buffer
    float fog_density;                // Extinction override (0 = scene-scaled)
    float fog_height;                 // Height falloff override (0 = scene-scaled)
    float fog_anisotropy;             // Scatter anisotropy (-999 = keep engine default)
    int albedo_debug;                 // Show the resolved albedo G-buffer
    int no_normals_mrt;               // Disable the normals G-buffer
    int normals_debug;                // Show the resolved normals G-buffer
    int no_ssr;                       // Disable screen-space reflections
    int no_ssr_full_res;              // Trace SSR at half res (the old, serrated path)
    int no_ssr_temporal;              // Disable SSR temporal accumulation (raw single-frame march)
    int no_ssr_denoise;               // Disable the SSR denoiser (deterministic march, no jitter)
    float ssr_jitter;                 // SSR stochastic ray-jitter spread override (-1 = default)
    int ssr_debug;                    // Show the reflection buffer
    float ssr_strength;               // SSR strength override (-1 = default)
    float specular_aa;                // Specular AA strength override (-1 = default)
    int no_energy_comp;               // Disable multi-scatter energy compensation
    int no_refraction;                // Disable screen-space refraction
    int no_clearcoat;                 // Disable the clearcoat second specular lobe
    int no_specular;                  // Disable KHR_materials_specular F0 tint + weight
    int no_sheen;                     // Disable KHR_materials_sheen cloth lobe
    int no_parallax;                  // Disable parallax occlusion mapping (POM)
    float parallax_scale;             // POM depth override (< 0 = keep engine default)
    int no_sss;                       // Disable separable subsurface scattering
    int oit;                          // Enable weighted-blended OIT (default off)
    int point_light_grid;             // N: spawn an NxN point-light test grid (0 = off)
    float plg_radius;                 // Grid spacing == per-light cull radius
    float plg_intensity;              // Grid light intensity
    float sss_radius;                 // SSS scatter radius override (< 0 = fixture default)
    float sss_color[3];               // SSS scatter color override (< 0 in [0] = fixture default)
    int no_bloom;                     // Disable bloom
    int bloom_enable;                 // -1 = keep default; 0/1 force (scene file)
    float bloom_strength;             // -1 = keep engine default
    float bloom_threshold;            // -1 = keep engine default
    float ibl_intensity;              // -1 = keep engine default
    int no_scene_file;                // Ignore any .cscn (input still allowed, look skipped)
    int tonemap_mode;                 // PostFXTonemapMode override (0 = keep default;
                                      // coincides with PASSTHROUGH, which is a blit
                                      // path and never user-set)
    int ssaa;                         // Supersampling factor (0 = keep engine default)
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
    int headless_jitter; // Apply TAA jitter in headless (lets temporal effects converge)
    int max_frames;      // Exit after this many frames (0 = run forever)
    int show_bones;
    int check_stretch; // One-shot CPU skinning stretch diagnostic
    int render_mode;   // RenderMode override for debugging (-1 = PBR)
    float orbit_yaw;   // Camera yaw around the model in degrees (0 = front)
    float orbit_pitch; // Camera pitch in degrees (0 = level, 90 = top-down)
    // Explicit camera pose (reproduces any interactive view; overrides the
    // yaw/pitch/distance orbit framing). --cam-eye and --cam-target must both
    // be given; --cam-up is optional (default 0,1,0). Print them from the GUI
    // "Print Camera" button.
    int cam_eye_set;     // --cam-eye given
    float cam_eye[3];    // Explicit eye position (world)
    int cam_target_set;  // --cam-target given
    float cam_target[3]; // Explicit look-at target (world)
    int cam_up_set;      // --cam-up given
    float cam_up[3];     // Explicit up vector (default 0,1,0)
    int show_help;
} RenderArgs;

#endif // RENDER_ARGS_H
