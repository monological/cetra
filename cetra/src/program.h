#ifndef _PROGRAM_H_
#define _PROGRAM_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "shader.h"
#include "uniform.h"

#include "ext/uthash.h"

// An app shader's own uniforms (spec 13.29): each a name and a vec4, uploaded under that name
// wherever their owner's program is bound -- a material's, a post pass's. Eight because a
// shader with more than eight knobs wants a texture; the name is bounded so an entry costs no
// allocation.
#define SHADER_PARAM_MAX  8
#define SHADER_PARAM_NAME 32
typedef struct ShaderParam {
    char name[SHADER_PARAM_NAME];
    vec4 value;
} ShaderParam;
typedef struct ShaderParams {
    ShaderParam list[SHADER_PARAM_MAX];
    int count;
} ShaderParams;

// Set a param by name, adding it the first time. False, logged, when SHADER_PARAM_MAX others are
// already held or the name is empty or does not fit.
bool shader_params_set(ShaderParams* params, const char* name, const vec4 value);
// Upload every param to `uniforms`, the bound program's. A name the program does not declare
// costs a location lookup and nothing else. Nothing resets a name an owner leaves out, so two
// owners sharing one program each set every param it reads, or the second inherits the first's.
void shader_params_upload(const ShaderParams* params, UniformManager* uniforms);

// Which VERTEX stage a lit-surface variant is built on (spec 11.95). Up here
// only because ShaderProgram carries one; the family's rationale and the rest of
// the variant API are together further down.
typedef enum PbrFamily {
    PBR_FAMILY_RIGID = 0, // pbr_vert: instanced, the default for every app
    PBR_FAMILY_SKINNED,   // pbr_skinned_vert: bone_matrices, one draw per mesh
} PbrFamily;

typedef struct ShaderProgram {
    GLuint id;
    char* name;
    Shader** shaders;
    size_t shader_count;
    UniformManager* uniforms;
    // Whether this program reads its per-object transforms from InstanceBlock,
    // resolved from the linked program (ubo_wire_blocks). False means a draw
    // must carry exactly one object: the transform arrives as a plain uniform,
    // so every instance of a batched draw would land on the first one's.
    bool instanced;
    // Whether the depth prepass may draw this program's meshes. TWO
    // requirements, and they are separate even though today's two flagged
    // programs happen to satisfy both:
    //
    //   1. gl_Position comes from object_position.glsl, so the lean
    //      depth_prepass_vert lands on the same value and the shading pass
    //      survives GL_LEQUAL against it. This is what the flag was named for.
    //   2. The FRAGMENT stage implements passMode == SUBMIT_PASS_DEPTH_ONLY,
    //      because masked meshes are prepassed through this program rather than
    //      the lean one (spec 11.31). A program satisfying (1) but not (2) gets
    //      its passMode upload silently swallowed -- uniform writes are
    //      location-guarded -- and the prepass then stamps depth across the whole
    //      quad with no coverage test, punching a hole in whatever is behind.
    //
    // Both hold only because `pbr` and `pbr_skinned` are the sole setters and
    // both use pbr_frag. Anything setting this for reason (1) alone must either
    // carry pbr_frag or be kept out of the masked sweep.
    //
    // Opt-IN, because the failure is silent and ugly. `shape` is the standing
    // counter-example: it is GL_LINES plus a geometry shader that expands each
    // segment into a screen-facing quad, and its vertex stage emits a WORLD
    // position for the geometry stage to re-project. Prepassing it would stamp
    // depth along unexpanded lines that nothing ever shades. Its meshes are
    // ALPHA_OPAQUE, so the lane alone cannot tell them apart.
    bool depth_prepass_safe;
    // Draws a furred mesh as its skin and the shells of its coat, one instance each, with
    // gl_InstanceID as the shell (spec 13.17). Set by the variant builder on the skinned stage
    // carrying the fur bit -- which is also what makes such a variant unsafe to prepass: the
    // shell code changes the position the prepass would have to match.
    bool fur_shells;
    // Built by create_late_surface_program (spec 13.29): its vertex stage is the late draw's and
    // it writes the canvas rather than the G-buffer, so only a MATERIAL_PASS_LATE_DRAW material
    // may carry it, and such a material carries nothing else.
    bool late_surface;
    // Which lit-surface features this program was compiled with, or -1 for a
    // program that is not a variant at all (spec 11.93). A third derived fact
    // beside the two above, and for their reason: it is settled at build time,
    // and every reader wants the answer rather than the derivation.
    //
    // It replaced a string test -- the resolver formatted "pbr-<mask>" and
    // strcmp'd it against this program's name every frame, per material. That
    // made the program NAMESPACE the membership rule, so any future program
    // called "pbr-anything" would have been silently swapped for a variant.
    int pbr_features;
    // How many sampler uniforms the LINKED program kept, which is not how many
    // its source declares. GL bounds the first against GL_MAX_TEXTURE_IMAGE_UNITS
    // (16 on this platform's guarantee) and says nothing about the second, so a
    // declaration whose every read was compiled away may or may not still be
    // spending a unit -- that is a question about the driver, and this field is
    // how it gets asked.
    //
    // -1 when the count could not be taken, so a reader cannot mistake "not
    // measured" for "declares nothing".
    int sampler_count;
    // The primitive a geometry stage declares as its input (GL_LINES,
    // GL_TRIANGLES, ...), or -1 for a program with no geometry stage, which
    // takes any -- not 0, which is GL_POINTS. The one fact that decides whether
    // a mesh's draw mode can be drawn by this program at all; a mismatch is
    // GL_INVALID_OPERATION at the draw, which nothing reads.
    GLint geometry_input;
    // Which vertex stage this variant was built on, so the resolver can swap a
    // material within its own family (spec 11.95).
    //
    // Its zero IS meaningful here, unlike pbr_features' -- but harmlessly, since
    // every read is already behind `pbr_features >= 0` and a program that is not
    // a variant never reaches one.
    PbrFamily pbr_family;
    // The surface hook spliced into this variant (spec 13.29), NULL for none. The resolver keeps
    // a material on its hook by comparing this with the material's, as it compares the mask.
    const struct ShaderHook* pbr_hook;
    UT_hash_handle hh;
} ShaderProgram;

/*
 * Shader Program Functions
 */
ShaderProgram* create_program(const char* name);
ShaderProgram* create_program_from_paths(const char* name, const char* vert_path,
                                         const char* frag_path, const char* geo_path);
ShaderProgram* create_program_from_source(const char* name, const char* vert_source,
                                          const char* frag_source, const char* geo_source);
void free_program(ShaderProgram* program);

// Hot-reload: recompile shaders from paths and relink program
GLboolean reload_program_from_paths(ShaderProgram* program, const char* vert_path,
                                    const char* frag_path, const char* geo_path);

/*
 * Shader Program Setup Functions
 */
void attach_shader_to_program(ShaderProgram* program, Shader* shader);
GLboolean link_program(ShaderProgram* program);
GLboolean validate_program(ShaderProgram* program);
void setup_program_uniforms(ShaderProgram* program);

// Whether a mesh drawn as `draw_mode` (a GL primitive; MeshDrawMode's values)
// can go through this program: true for a program with no geometry stage,
// else only when the mode belongs to the family the stage declared.
bool program_accepts_draw_mode(const ShaderProgram* program, GLenum draw_mode);

/*
 * Preset Programs
 */
/*
 * Lit-surface variants (spec 11.93).
 *
 * A feature a material cannot use still costs it, and the cost is OCCUPANCY
 * rather than work: every gate in pbr_frag is a dynamically uniform branch that
 * an unusing scene already skips, so what is left is live values and declared
 * samplers that every fragment pays for. Measured on forest, whose materials use
 * none of these five: removing them at compile time took the opaque pass from
 * 149.2 to 125.1 ms and the frame from 203.7 to 179.0. Guarding one of the same
 * code paths at RUNTIME instead was worth 0.05 ms, which is the same fact from
 * the other side.
 *
 * The polarity is SUBTRACTIVE -- the defines turn features OFF, and no defines
 * is exactly today's shader. So a resolver that fails to run, or a mask that is
 * never assembled, yields the slow program rather than a fast one missing a
 * feature the material needed. Wrong-and-slow is recoverable; wrong-and-pretty
 * is what ships.
 *
 * SCENE-scoped and MATERIAL-scoped bits both live here, and they are not the
 * same kind of thing. Decals and area lights are properties of the scene -- a
 * material cannot know whether either exists -- while sheen, anisotropy and
 * parallax are the material's own. The mask is the union, so a scene that gains
 * its first decal changes every material's variant.
 */
// The bits, from the file the shader reads them out of. Included rather than
// restated so a mask emitted here cannot mean something else there.
#include "../shaders/include/pbr_features.glsl"

// DECALS, AREA and RAIN are the scene's, SHEEN, ANISO, PARALLAX and LAYERS the
// material's -- which is the split _scene_pbr_features and _material_pbr_features
// are named for. RELIEF is the material's, asked only in a scene that rains. A
// material cannot know whether the scene has a decal, so the mask is the union of
// the two and a scene gaining its first decal changes every material.

// PbrFamily (declared above, beside the struct that carries one) is which VERTEX
// stage a variant is built on. The two families share pbr_frag EXACTLY, so a
// mask means the same thing in both and the bits above need no per-family
// reading -- which is the whole reason a second family costs a builder argument
// rather than a second set of gates.
//
// It is a family and not a flag because the vertex source, the cache-key prefix
// and the resulting `instanced` answer all follow from it together: a skinned
// program links without an InstanceBlock, which is what keeps 11.28's rule that
// such a program may never carry more than one instance.

// The names create_engine registers for apps, for engine_get_program. A literal
// at a call site is a typo waiting; these are the four an app has reason to
// ask for, plus the particle program an app creates and registers itself.
// pbr_variant_name builds on the first two, so the cache key and the lookup
// cannot drift apart.
#define CETRA_PROGRAM_PBR         "pbr"
#define CETRA_PROGRAM_PBR_SKINNED "pbr_skinned"
#define CETRA_PROGRAM_SHAPE       "shape"
#define CETRA_PROGRAM_XYZ         "xyz"
#define CETRA_PROGRAM_PARTICLE    "particle"
// Registered at creation like the rest, though not one an app has reason to ask for.
#define CETRA_PROGRAM_RAIN "rain"

// Longest "pbr_skinned-<mask>-h<hook id>" plus its terminator, with room to spare.
#define PBR_VARIANT_NAME_MAX 48

struct ShaderHook;

// The cache key for a variant, and the ONE place the family-and-mask-to-name
// rule lives. Two sites spelling it differently would miss the lookup forever,
// compiling and leaking a fresh program every frame while rendering correctly
// throughout. A hooked variant (spec 13.29) carries its hook's id as well, so two
// hooks at one mask are two programs.
void pbr_variant_name(PbrFamily family, unsigned features, const struct ShaderHook* hook, char* out,
                      size_t n);

// Compile the variant carrying exactly `features` on `family`'s vertex stage,
// with `hook`'s GLSL spliced in when it is not NULL. Does not register it -- see
// engine_pbr_variant, which owns the cache and is where callers should go.
ShaderProgram* create_pbr_program_variant(PbrFamily family, unsigned features,
                                          const struct ShaderHook* hook);

// The full variant of each family, which is the uber-shader and what an app
// hands to node_set_programs before the resolver narrows it.
ShaderProgram* create_pbr_program();
ShaderProgram* create_pbr_skinned_program();

// Particle Program (instanced camera-facing billboards)
ShaderProgram* create_particle_program();
// Falling rain's streaks (spec 13.9): instanced, with no vertex data at all
ShaderProgram* create_rain_program();
// Fire (spec 13.14): a GRID fire's simulation passes, the reduction that reads back what it
// casts, and the debug slice; the march that draws a GRID or FLAME fire, and a flipbook's cards.
ShaderProgram* create_fire_advect_program();
ShaderProgram* create_fire_correct_program();
ShaderProgram* create_fire_curl_program();
ShaderProgram* create_fire_react_program();
ShaderProgram* create_fire_divergence_program();
ShaderProgram* create_fire_jacobi_program();
ShaderProgram* create_fire_project_program();
ShaderProgram* create_fire_reduce_program();
ShaderProgram* create_fire_sum_program();
ShaderProgram* create_fire_slice_program();
ShaderProgram* create_fire_march_program();
ShaderProgram* create_fire_card_program();
// Particle GPU-sim UPDATE program (transform feedback; vertex-only, spec 5.2)
ShaderProgram* create_particle_sim_program();
// Captures windOffset itself (transform feedback; vertex-only, spec 11.54), so
// the bound that lets a swaying mesh be culled is checked against the shader
// rather than against a second copy of it.
ShaderProgram* create_wind_probe_program();
ShaderProgram* create_shape_program();
ShaderProgram* create_xyz_program();
ShaderProgram* create_shadow_depth_program();
// The shadow depth program (or, with `absorb`, the translucent absorb program) carrying a surface
// hook (spec 13.29): its offset where the caster is placed, and in the depth program its alpha
// where the caster is cut. Built by the shadow system on a hooked caster's first draw.
ShaderProgram* create_shadow_hook_program(const struct ShaderHook* hook, bool absorb);
// Position only, for the depth prepass (spec 11.30). Shares the object-position
// chunk with pbr_vert so the two agree to the bit, which GL_LEQUAL against its
// output depends on.
ShaderProgram* create_depth_prepass_program();
// Water surface (spec 11.32). Its own program rather than a pbr_frag feature
// because it samples the resolved scene depth, and pbr_frag has declared all
// sixteen fragment samplers the driver allows.
ShaderProgram* create_water_program();
// The spectral cascade passes (--water-waves fft): evolve the spectrum, then one
// Stockham inverse-FFT stage per draw. Both write two MRT targets.
ShaderProgram* create_water_spectrum_program();
ShaderProgram* create_water_fft_program();
// Foam accumulation (spec 11.42): one pass a frame over the transformed cascades, holding
// whitewater on the surface after the crest that made it has passed.
ShaderProgram* create_water_foam_program();
// The surface query (spec 13.1): one texel per query point, the surface height over it.
ShaderProgram* create_water_probe_program();
// Refracted-grid caustics (spec 13.2): where each lattice corner's light lands, then the lattice
// drawn there additively.
ShaderProgram* create_water_caustic_land_program();
ShaderProgram* create_water_touch_program();
ShaderProgram* create_water_touch_normals_program();
ShaderProgram* create_water_caustic_program();
// Resolves the depth cascades into the filterable moment cascades (--msm)
ShaderProgram* create_msm_resolve_program();
ShaderProgram* create_shadow_absorb_program();
ShaderProgram* create_tsm_resolve_program();

// IBL Programs
ShaderProgram* create_skybox_program();
ShaderProgram* create_ibl_equirect_to_cube_program();
ShaderProgram* create_ibl_irradiance_program();
ShaderProgram* create_ibl_prefilter_program();
ShaderProgram* create_ibl_charlie_prefilter_program();
ShaderProgram* create_ibl_brdf_program();

// Sky atmosphere LUT programs
ShaderProgram* create_sky_transmittance_program();
ShaderProgram* create_sky_multiscatter_program();
ShaderProgram* create_sky_debug_program();
ShaderProgram* create_sky_view_program();
ShaderProgram* create_sky_env_program();
ShaderProgram* create_sky_background_program();
// Aerial-perspective volume, one draw per slice (spec 9.6)
ShaderProgram* create_sky_aerial_program();
// Cloud-noise volume slice inspector (spec 11.0)
ShaderProgram* create_cloud_noise_debug_program();
// Half-res cloud shell march (spec 11.0)
ShaderProgram* create_cloud_march_program();
ShaderProgram* create_cloud_shadow_program();
// Sky background with the cloud composite (bound only when clouds are on)
ShaderProgram* create_sky_background_clouds_program();
// Env-cubemap face render with the low-quality cloud march (release bakes)
ShaderProgram* create_sky_env_clouds_program();

// Copies/resamples a 2D mask texture into a material-mask-array layer
ShaderProgram* create_mask_copy_program();
ShaderProgram* create_layers_vt_bake_program();
ShaderProgram* create_layers_vt_feedback_program();

// Text Program
ShaderProgram* create_text_program();
// The UI overlay's program (spec 12.2). One vertex format carries flat fills,
// textured quads and SDF glyphs, so a screen is one or two draws rather than
// one per element.
ShaderProgram* create_ui_program();

// Bone Visualization Program
ShaderProgram* create_bone_program();

// Shadow Catcher Program
ShaderProgram* create_shadow_catcher_program();

// An app's fullscreen fragment stage over the engine's own vertex stage (spec 13.29), which
// hands it `in vec2 TexCoords`, 0..1 across the target. What postfx_add_pass runs.
ShaderProgram* create_post_pass_program(const char* name, const char* frag_source);
// The finished picture into the window, dithered, when something drew after the tone map.
ShaderProgram* create_present_program();

/*
 * An app's fragment stage for a material drawn in the late draw (spec 13.29), over the engine's
 * late_surface_vert, which hands it `vWorldPos`, `vNormal` (world), `vUv`, `vColor` (the vertex
 * colour, (0,0,0,1) where the mesh has none) and `vViewDepth` (metres in front of the eye). It
 * writes `FragColor` PREMULTIPLIED onto the canvas -- blend ONE, ONE_MINUS_SRC_ALPHA, so alpha 0
 * adds light and alpha 1 replaces -- and include/late_surface.glsl carries what it needs from the
 * frame: the depth test, the fog and the pre-exposure.
 */
ShaderProgram* create_late_surface_program(const char* name, const char* frag_source);

// Post-Processing Programs
ShaderProgram* create_bloom_bright_program();
// Diffraction glare (spec 13.4): the bright source, one FFT stage, the spectrum multiply and
// the unpacking into the frame.
ShaderProgram* create_glare_source_program();
ShaderProgram* create_glare_fft_program();
ShaderProgram* create_glare_multiply_program();
ShaderProgram* create_glare_output_program();
// The CRT (spec 13.28): the picture resampled to its signal, and the television drawn from it.
ShaderProgram* create_crt_resample_program();
ShaderProgram* create_crt_program();
// Local exposure (spec 13.19): the half-res frame, the bilateral grid's build and blur, and the
// blurred luminance's blocks and Gaussian.
ShaderProgram* create_le_half_program();
ShaderProgram* create_le_bins_program();
ShaderProgram* create_le_grid_program();
ShaderProgram* create_le_grid_blur_program();
ShaderProgram* create_le_block_sum_program();
ShaderProgram* create_le_block_program();
ShaderProgram* create_le_blur_program();
ShaderProgram* create_bloom_down_program();
ShaderProgram* create_bloom_up_program();
ShaderProgram* create_lens_flare_program();
ShaderProgram* create_tonemap_program();
ShaderProgram* create_spec_occ_composite_program();
ShaderProgram* create_gtao_program();
ShaderProgram* create_ssao_blur_program();
ShaderProgram* create_ssr_program();
ShaderProgram* create_ssr_hiz_program();
ShaderProgram* create_upsample_tent_program();
ShaderProgram* create_ssr_fold_wet_program();
ShaderProgram* create_taa_resolve_program();
// TAAU render-to-post upscaling resolve (render_scale < 1 only)
ShaderProgram* create_taau_resolve_program();
ShaderProgram* create_temporal_accum_program();
ShaderProgram* create_ssgi_composite_program();
ShaderProgram* create_ssgi_accum_program();
ShaderProgram* create_ssgi_atrous_program();
ShaderProgram* create_ssr_atrous_program();
ShaderProgram* create_ssr_accum_program();
ShaderProgram* create_froxel_inject_program();
ShaderProgram* create_froxel_integrate_program();
ShaderProgram* create_froxel_composite_program();
// Downsamples the scene's depth cascades into the fog's own filterable
// exponential representation (spec 11.12); one pass per layer per axis.
ShaderProgram* create_fog_esm_program();
ShaderProgram* create_gi_project_program();
ShaderProgram* create_probe_project_program();
ShaderProgram* create_lum_measure_program();
ShaderProgram* create_lum_histogram_program();
ShaderProgram* create_lum_reduce_program();
ShaderProgram* create_dof_coc_program();
ShaderProgram* create_dof_tile_program();
ShaderProgram* create_dof_dilate_program();
ShaderProgram* create_dof_gather_program();
ShaderProgram* create_dof_composite_program();
ShaderProgram* create_motion_blur_program();
ShaderProgram* create_motion_blur_tilemax_program();
ShaderProgram* create_motion_blur_neighbormax_program();
ShaderProgram* create_sss_gather_program();
ShaderProgram* create_sss_pyr_seed_program();
ShaderProgram* create_sss_pyr_down_program();
ShaderProgram* create_contact_shadow_program();
ShaderProgram* create_oit_resolve_program();

#endif // _PROGRAM_H_
