#ifndef RENDER_ARGS_H
#define RENDER_ARGS_H

/*
 * Command line arguments / merged scene-file values. CLI flags win; scene
 * files (.cscn) fill fields the CLI left at their sentinels (see
 * cscene_apply.c); the application blocks in main read only this struct.
 */

#define MAX_ANIM_FILES 32

// Entries in the --render-scale-at diagnostic schedule
#define RENDER_SCALE_AT_MAX 64

typedef struct {
    const char* model_path;
    const char* texture_dir;
    const char* hdr_path;
    const char* anim_files[MAX_ANIM_FILES];
    int anim_count;
    const char* source_skeleton_path; // Source skeleton for retargeting
    // IESNA LM-63 file applied to every point and spot light, overriding what
    // the scene authored. A `.cscn` is the only way to attach a profile
    // otherwise, so without this the feature cannot be pointed at a real
    // luminaire without editing a committed asset first.
    const char* ies_profile_path;
    const char* screenshot_path; // Save final frame here (PPM)
    int screenshot_every;        // Also save numbered frames every N frames
    float fov_deg;               // Camera FOV in degrees (0 = default 50)
    float exposure;              // Tonemap exposure override; see has_exposure
    // Presence flag rather than testing `exposure > 0`, which six sites did.
    // Under a physical camera the value is an EV BIAS, where negative is a legal
    // and useful setting -- stopping down -- so sign cannot double as presence.
    int has_exposure;
    float ground_radius;   // Skybox ground projection dome radius (0 = default)
    float ground_height;   // HDR capture height above ground (0 = default)
    float camera_distance; // Camera distance override in meters (0 = auto)
    int no_recenter;       // Keep the model's authored world position
    // Tri-state so "unspecified" is distinct from "off": -1 unset, 0 pin the
    // frame, 1 keep adapting. Two booleans could not say "unset", which is what
    // lets an authored exposure imply a pin only when nothing asked otherwise.
    int auto_exposure_override;
    // Physical camera from post.camera; aperture <= 0 means unset, so exposure
    // stays the linear multiplier it has always been.
    float aperture;
    float shutter_speed;
    float iso;
    int no_flip_uv;             // For assets baked with the opposite V convention
    float ao_radius;            // AO/GI reach override in world units (0 = auto)
    int force_taa;              // TAA even in headless (temporal passes active)
    int msaa;                   // Requested MSAA sample count; 0 = the
                                // TAA/headless policy decides
    int no_ground;              // Disable skybox ground projection
    int no_key_light;           // Pure IBL: skip the analytic key lights
    int no_shadows;             // Keep key lights but disable shadow maps
    int no_pcss;                // Fixed-width PCF instead of contact-hardening
    int translucent_shadows;    // Per-texel transmittance for hair/glass/foliage casters
    int no_translucent_shadows; // Force it off (the off-path and inverse arms)
    int profiler_enabled;       // Per-pass GPU + CPU time and submission counts
    int no_instancing;          // Force one draw per mesh (the identity arm)
    int no_frustum_cull;        // Submit every item, culled or not (the bisect lever)
    int no_sort_opaque;         // Draw the opaque lane in graph order (default is sorted)
    int depth_prepass;          // Position-only depth for non-masked opaques (default off)
    int no_lod;                 // Draw every mesh at LOD level 0
    float lod_bias;             // LOD distance bias (0 = keep engine default)
    int msm;                    // Moment shadow maps; excludes PCSS
    int msm_size;               // Moment cascade edge (0 = keep engine default)
    float msm_blur;             // Moment blur spacing in texels (-1 = default)
    float msm_bleed;            // Moment leak cutoff (-1 = default)
    float light_size;           // Emitter size override (-1 = scene default)
    float shadow_softness;      // PCSS softness override (-1 = default)
    int shadow_cascades;        // Cascades per caster (0 = keep engine default)
    // World point the scene-fit shadow map is built around. 0 = the engine
    // default, which is the origin; 1 = the explicit vector below; 2 = the
    // scene's own bounds centre. A scene authored away from the origin needs
    // this or it falls out of the map, and no other lever reaches the field.
    int shadow_center_mode;
    float shadow_center[3];
    int csm_debug;         // Tint fragments by selected cascade
    int no_springs;        // Disable spring-bone secondary motion
    int no_ssao;           // Disable screen-space ambient occlusion
    int ssao_debug;        // Show the raw SSAO buffer
    int spec_occ_mode;     // PostFXSpecOccMode override (-1 = keep engine default)
    int spec_occ_debug;    // Show the AO visibility the scene is multiplied by
    int bent_debug;        // Show the bent normal from the AO chain
    int no_ao_edge_filter; // Disable the depth-aware AO blur (allow silhouette bleed)
    int ssgi;              // Enable screen-space GI (indirect diffuse)
    int ssgi_debug;        // Show the raw gathered GI radiance
    int probe;             // Enable the local reflection probe
    int probe_pos_set;     // --probe-pos given
    float probe_pos[3];    // Probe capture position override
    int probe_scene;       // Capture the scene meshes too (interiors)
    int probe_debug;       // Show the raw capture as the background
    int gi_volume;         // Enable the DDGI irradiance probe volume
    int gi_probes[3];      // Probe grid counts (0,0,0 = default)
    int water;             // Enable the water surface (spec 11.32)
    float water_level;     // Still-water plane, world Y (-9999 = keep default)
    float water_extent;    // Half-size of the shoaling bed's domain (0 = keep default)
    int water_waves;       // Wave model: -1 = unset, 0 = Gerstner, 1 = spectral
    int no_water_caustics; // Bisect lever: drop the surface's light focusing
    int no_water_glitter;  // Bisect lever: drop the analytic sun lobe
    // WaterFoamDebug (water.h), as an int since args structs here don't carry engine
    // headers. The instrument whitecap coverage is measured with; see Water.foam_debug.
    int water_foam_debug;
    int no_water_surf;          // Bisect lever: no incident wave at the shore
    int no_water_foam_history;  // Bisect lever: foam from this frame's fold only
    int no_water_coverage;      // Bisect lever: hard shoreline cutoff, no coverage
    int no_water_lod;           // Bisect lever: full wave detail at any cell footprint
    int no_water_wetness;       // Bisect lever: the swash wets no material it runs over
    int no_water_film;          // Bisect lever: the closed-form run-up, with no swash solver
    int shore_probe;            // Print the CPU twin of the run-up, the film's own drive
    int water_bed_dome;         // Install the analytic dome bed provider (shoaling)
    int no_water;               // Drop a water surface a scene file asked for
    int water_probe;            // Print the CPU wave query over a grid, then continue
    int water_fft_probe;        // Print the transformed spectrum's measured statistics
    int wind_bound_probe;       // Print the measured wind displacement beside its cull bound
    int ies_probe;              // Print every loaded IES profile and a sweep of its angles
    int emissive_lights;        // Derive an LTC area panel from every emissive mesh
    int emissive_light_probe;   // Print the area panel every emissive mesh would derive
    int exposure_probe;         // Print what the meter decided, per metered frame
    float meter_low;            // Metering low percentile (<0 = leave the default)
    float meter_high;           // Metering high percentile (<0 = leave the default)
    int meter_mode;             // MeteringMode override (-1 = leave the default)
    float meter_radius;         // Spot / centre-weight radius (<0 = leave the default)
    float adapt_up;             // Per-frame adaptation rate, scene brightening (<0 = default)
    float adapt_down;           // Per-frame adaptation rate, scene darkening (<0 = default)
    int gi_rate;                // Probes captured per frame while dirty (0 = default)
    int gi_debug;               // Blit the probe atlas into the frame corner
    int sky;                    // Procedural physically-based sky instead of -e
    int sky_debug;              // Blit the sky LUTs into the frame corner
    int clouds;                 // Volumetric cloud layer (implies --sky)
    int no_fog_volumes;         // Drop any fogVolumes[] a scene file authored. The only way
                                // off, mirroring --no-water: a volume has no CLI counterpart
                                // to omit, so authoring is the only way on.
    int no_cloud_shadows;       // Keep the deck, drop the shadow it casts into the fog. A
                                // bisect lever rather than a feature: the shadow is on with
                                // the clouds, because a cloud that casts none is wrong.
    float cloud_coverage;       // 0..1; negative = keep the engine default
    float cloud_density;        // extinction scale; negative = default
    float cloud_wind_kmh;       // drift speed; negative = default (still)
    float cloud_wind_deg;       // drift direction; negative = default
    float sun_elevation;        // Sky sun elevation in degrees (-999 = default)
    float sun_azimuth;          // Sky sun azimuth in degrees (-999 = default)
    float world_scale;          // World units per km for the atmosphere (-1 = default)
    int flip_uv;                // Force the UV V-flip ON (asset baked opposite to format default)
    int no_unit_scale;          // Skip import unit normalization (raw file units)
    float import_scale;         // Extra uniform scale on top of unit normalization (1 = none)
    int no_aerial;              // Disable aerial perspective (on by default with --sky)
    int sky_rebake_stress;      // Diagnostic: N headless sun re-bakes then restore
    int fog;                    // Enable volumetric fog
    float fog_density;          // Extinction override (0 = scene-scaled)
    float fog_height;           // Height falloff override (0 = scene-scaled)
    float fog_anisotropy;       // Scatter anisotropy (-999 = keep engine default)
    float fog_near;             // Volume near (-1 = keep engine default; 0 derives from far)
    float fog_far;              // Volume far (-1 = keep engine default)
    float fog_depth_dist;       // Slice bias exponent (-1 = keep engine default)
    int contact_shadows;        // Enable screen-space contact shadows
    int contact_shadows_debug;  // Show the raw contact-shadow visibility term
    float cs_distance;          // March reach override (-1 = scene-scaled)
    float cs_strength;          // Darkening weight override (-1 = engine default)
    int albedo_debug;           // Show the resolved albedo G-buffer
    int no_normals_mrt;         // Disable the normals G-buffer
    int normals_debug;          // Show the resolved normals G-buffer
    int no_ssr;                 // Disable screen-space reflections
    int no_ssr_full_res;        // Trace SSR at half res (the old, serrated path)
    int no_ssr_temporal;        // Disable SSR temporal accumulation (raw single-frame march)
    int no_ssr_denoise;         // Disable the SSR denoiser (deterministic march, no jitter)
    float ssr_jitter;           // SSR stochastic ray-jitter spread override (-1 = default)
    int ssr_debug;              // Show the reflection buffer
    float ssr_strength;         // SSR strength override (-1 = default)
    float specular_aa;          // Specular AA strength override (-1 = default)
    int no_energy_comp;         // Disable multi-scatter energy compensation
    int no_refraction;          // Disable screen-space refraction
    int no_clearcoat;           // Disable the clearcoat second specular lobe
    int no_specular;            // Disable KHR_materials_specular F0 tint + weight
    int no_sheen;               // Disable KHR_materials_sheen cloth lobe
    int no_parallax;            // Disable parallax occlusion mapping (POM)
    float parallax_scale;       // POM depth override (< 0 = keep engine default)
    int no_sss;                 // Disable separable subsurface scattering
    int no_skin_preint;         // Disable pre-integrated skin diffuse (§11.13)
    float curvature_scale;      // Pre-integration strength override (< 0 = keep the material's)
    int oit;                    // OIT for blend meshes; -1 unset, 0 off, 1 on (engine default ON)
    int oit_moments;            // Absorbance-moment weight in the accumulate; same tri-state
    int no_layers_vt;           // Per-texel blend on every layered material (cache off)
    int layers_vt_res;          // Composite-cache resolution override; 0 = derived
    int show_lights;            // Draw light gizmos (position + cull radius)
    int cluster_heatmap;        // Tint by cluster light count
    int area_light;             // --area-light given: spawn one LTC panel
    float area_light_pos[3];    // Panel center (world)
    float area_light_dir[3];    // Panel normal; it lights the side this points at
    float area_light_size[2];   // Panel width x height (world units)
    float area_light_intensity; // Emitted radiance
    float area_light_color[3];  // Panel tint (default white)
    int point_light_grid;       // N: spawn an NxN point-light test grid (0 = off)
    float plg_radius;           // Grid spacing == per-light cull radius
    float plg_intensity;        // Grid light intensity
    float sss_radius;           // SSS scatter radius override (< 0 = fixture default)
    float sss_color[3];         // SSS scatter color override (< 0 in [0] = fixture default)
    int no_bloom;               // Disable bloom
    int bloom_enable;           // -1 = keep default; 0/1 force (scene file)
    float bloom_strength;       // -1 = keep engine default
    float bloom_threshold;      // -1 = keep engine default
    float ibl_intensity;        // -1 = keep engine default
    int no_scene_file;          // Ignore any .cscn (input still allowed, look skipped)
    int tonemap_mode;           // PostFXTonemapMode override (0 = keep default;
                                // coincides with PASSTHROUGH, which is a blit
                                // path and never user-set)
    int ssaa;                   // Supersampling factor (0 = keep engine default)
    float render_scale;         // TAAU render-res scale [0.5, 1) (0 = full res)
    // Diagnostic render-scale schedule (--render-scale-at): switch to
    // scale_at_value[i] on frame scale_at_frame[i]. Empty on a normal run.
    int scale_at_count;
    int scale_at_frame[RENDER_SCALE_AT_MAX];
    float scale_at_value[RENDER_SCALE_AT_MAX];
    // Diagnostic (--shadows-off-at): clear shadow_system->enabled on this frame,
    // -1 for never. The GUI checkbox reaches the same field, and nothing else
    // headless does -- --no-shadows clears it before the first depth pass, so
    // every index the pass maintains is still at its initial value and the
    // TRANSITION, which is what leaves those indices stale, is unreachable.
    int shadows_off_at;
    // Finishing grade (-1 = keep engine default; >=0 enables + sets)
    int film_preset; // --film: enable the whole finishing stack at sane defaults
    float vignette;
    int no_vignette; // Force the default vignette off
    float dither;
    int no_dither; // Force the default output dither off
    float grain;
    float sharpen;
    float flare;     // Lens ghost strength
    float chromatic; // Channel separation at the corner, in pixels
    int grade_set;   // A grade component was given -> enable the grade
    float grade_lift[3];
    float grade_gamma[3];
    float grade_gain[3];
    // 3D colour-grading LUT (spec 11.58). A .cube from a colourist, applied
    // after the gamma encode -- the general map lift/gamma/gain cannot be,
    // since three per-channel knobs cannot rotate a hue or mix channels.
    const char* lut_path;
    // The only way to stop a LUT from LOADING -- and it cancels a --lut on the
    // same command line too. Not the only way off a grade: --lut-strength 0 is
    // a bit-exact identity, which the lut-strength arm asserts at 0 px.
    int no_lut;
    float lut_strength; // -1 = keep the default
    int lut_interp;     // -1 unset, else PostFXLutInterp
    int dof;            // --dof: enable depth of field
    int no_dof;         // Force DoF off (e.g. --film --no-dof)
    float dof_focus;    // Focus distance in view units (-1 = auto: subject)
    float dof_range;    // Ramp-to-full-blur width (-1 = scene-scaled default)
    float dof_max_coc;
    int dof_blades;          // Aperture blade count (-1 = keep engine default)
    float dof_rotation;      // Aperture rotation in degrees (< 0 = keep default)
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
