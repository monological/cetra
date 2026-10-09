#ifndef _ENGINE_INTERNAL_H_
#define _ENGINE_INTERNAL_H_

// The engine's pass brackets and frame plumbing: what render.c, the shadow
// pass, the captures and the post chain call between them. Nothing an app
// drives a frame with lives here; that is engine.h. Internal, so an app that
// includes it has said it is reaching into the frame.

#include "engine.h"
#include "uniform.h"

// The lit-surface variant of `family` carrying exactly `features`, compiled and
// registered on first ask and answered from the program cache afterwards
// (spec 11.93; the family since 11.95), with `hook`'s GLSL spliced in when it is
// not NULL (spec 13.29).
//
// Here rather than in program.c because it is cache management over
// engine->programs, and program.h cannot see an Engine -- engine.h includes it,
// not the other way round. The naming rule stays in program.c, where the builder
// that has to agree with it lives.
ShaderProgram* engine_pbr_variant(Engine* engine, PbrFamily family, unsigned features,
                                  const struct ShaderHook* hook);

// Take ownership of a surface hook (spec 13.29), freed with the engine by free_shader_hook. False,
// logged, when there is no memory to hold it, and the hook is still the caller's.
bool engine_add_shader_hook(Engine* engine, struct ShaderHook* hook);
void free_shader_hook(struct ShaderHook* hook);

/*
 * Every uniform object_position.glsl's displacers read, in one call.
 *
 * The chunk lands in FIVE programs -- pbr, pbr_skinned, the depth prepass, the
 * shadow depth pass and the shadow absorb pass -- and all five must displace a
 * vertex to the same place. A shadow that leaves its caster is the mild failure;
 * the prepass's one-sided LEQUAL is the sharp one, where a surface that moved
 * differently in the two passes DISAPPEARS rather than shading wrong.
 *
 * One function rather than a call per displacer at each site, because the
 * failure mode is arithmetic: it is not "an effect is missing" but "two passes
 * disagree", which no per-effect gate can see. Spec 11.62 shipped a wind uniform
 * that reached one program of the five and the full suite was green.
 *
 * `scene` may be NULL -- wind then uploads its own off state.
 */
void engine_upload_displacement_uniforms(const Engine* engine, const Scene* scene,
                                         UniformManager* u);

// Present the frame: resolve the MSAA framebuffer through the post stack
// (bloom + tone map, or a raw copy for non-PBR frame_mode) into the default
// framebuffer, then draw the GUI on top (auto-gated on the panel flags via
// gui_frame_active). Called by engine_run.
void engine_present_frame(Engine* engine, RenderMode frame_mode);

// The swap interval the engine's vsync and headless state ask for, onto the current context.
void engine_apply_swap_interval(const Engine* engine);

// The loading screen over the window, when it is shown (spec 13.34): the frame's draw of it, at
// the end of engine_present_frame, before the GUI; it is freed there once it has switched off.
// And its free, with the engine.
void loading_screen_frame(Engine* engine);
void free_loading_screen(LoadingScreen* ls);
// A draw between two pieces of long work inside a frame. It answers no window events, since one
// could resize the targets the frame is drawing into; draws at most once a refresh, so it may be
// called as often as is convenient; and does nothing with no screen shown. It puts back what it
// binds -- the program, the VAO, texture units 0 and 1, the colour mask, and the framebuffer,
// viewport, enables and blend state gl_pass_begin saves -- so a caller's GL state survives it.
void loading_screen_tick(const Engine* engine);
// Whether the screen hides the whole frame: shown, and not yet switching off as of the frame's
// own last draw, so every question of it in one frame has one answer. Such a frame draws no
// picture of its own, since nobody would see it.
bool loading_screen_covers(const Engine* engine);

// Select which color attachments the scene pass writes: attachment 0 only,
// or 0 + the view-space normals target (used by SSAO/SSR). Render passes
// that emit no normals (skybox, blend, overlays) switch to 0-only.
void engine_set_scene_draw_buffers(const Engine* engine, bool with_gbuffer);
// Mid-frame resolve of the opaque scene into the mipped refraction source
// (see the definition for the lifecycle); called by engine_render_scene
// between the skybox and the late pass when transmissive meshes exist.
bool engine_resolve_opaque_color(Engine* engine);
// Mid-frame resolve of the scene depth (from the multisample framebuffer) into
// a sampleable single-sample depth texture, for soft particles / other passes
// that need scene depth before postfx. Returns the depth texture (0 on failure),
// and re-binds engine->framebuffer so drawing can continue. Lazily sized to the
// render resolution.
GLuint engine_resolve_scene_depth(Engine* engine);

// Weighted-blended OIT accumulate sub-pass bracket: engine_begin_oit_pass binds
// the OIT FBO (accum + revealage, sharing the scene depth), clears it, and sets
// the independent per-target blend; engine_end_oit_pass restores the scene FBO,
// color-only draw buffer, and standard alpha blend. begin returns false if the
// targets couldn't allocate (caller falls back to the classic late pass).
bool engine_begin_oit_pass(Engine* engine);
void engine_end_oit_pass(Engine* engine);

// Moment generation sub-pass bracket (spec 11.17), run before the accumulate
// when moment weighting is on. Same shape as the accumulate bracket beside it:
// begin returns false if the targets couldn't allocate, and the caller falls
// back to the depth-curve weight.
bool engine_begin_moment_pass(Engine* engine);
void engine_end_moment_pass(Engine* engine);

// The render (not display) resolution: the scene target is supersampled by
// ss_scale, scaled by render_scale, and brought back to display size at the
// post chain's end, so this is the size the scene pass and every render-res
// post buffer actually rasterize into. Exported because callers outside
// engine.c kept re-deriving fb_size * ss_scale by hand, and a change to how
// render size is computed would silently miss them.
void engine_render_size(const Engine* engine, int* w, int* h);

// The post (pre-display) resolution: fb size x ss_scale, independent of
// render_scale -- what the chain downstream of the TAA seam runs at.
void engine_post_size(const Engine* engine, int* w, int* h);

#endif // _ENGINE_INTERNAL_H_
