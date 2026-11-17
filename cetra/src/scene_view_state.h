#ifndef _SCENE_VIEW_STATE_H_
#define _SCENE_VIEW_STATE_H_

#include <GL/glew.h>
#include <stdbool.h>
#include <stddef.h>
#include <cglm/cglm.h>

#include "common.h"
#include "exposure.h"
#include "postfx.h"
#include "scene_view.h"
#include "sky.h"
#include "ubo.h"

struct Engine;
struct SceneView;

// What the engine keeps for one view: its scene target, its post chain and meter, its camera's
// matrices this frame and last, and which of its G-buffer targets this frame writes. Internal: an
// app reads a view through SceneView.
typedef struct SceneViewState {
    struct Engine* engine; // the engine that made the view
    bool added;            // in the engine's list
    // A capture's view (a probe face, a GI probe face): no target or chain of its own, drawn into
    // the face the capture binds. It carries colour alone, single-sample, so a pass that reaches
    // outside the bound target or keys off a view's own targets sits it out.
    bool capturing;

    // The size the render targets were last built AT, or last attempted at. The frame-top sync
    // compares against these, so it both detects a change and declines to retry a size that
    // already failed.
    int target_render_w, target_render_h;
    int target_post_w, target_post_h;
    int target_out_w, target_out_h; // the view's size in framebuffer pixels
    // The rectangle in framebuffer pixels, GL's origin, as the targets were last fitted to it:
    // where the picture is shown, whatever the rectangle became after the view drew.
    int picture_rect[4];
    bool render_suspended; // a target rebuild failed; the view draws nothing until one
                           // at a size that allocates succeeds
    // What the driver returned for the sample count asked, read back from the scene FBO at every
    // build. Equal to the request at 1 by construction; above 1 it is the driver's answer.
    int msaa_samples_actual;
    int msaa_samples_built; // the count the targets were built at, clamped to the driver's

    GLuint framebuffer; // the scene MRT
    // The *_multisample_texture names predate spec 11.34: at one sample these are plain
    // GL_TEXTURE_2D targets, multisample only above that.
    GLuint multisample_texture;        // HDR color (attachment 0)
    GLuint normal_multisample_texture; // view-space normal .xyz + SSR reflective marker .a (1)
    GLuint aux_multisample_texture;    // motion .xy + linear view-Z .z + effective roughness .w (2)
    GLuint albedo_multisample_texture; // base color for SSGI (3)
    GLuint sss_diffuse_multisample_texture; // skin diffuse irradiance for SSS, 0 off-skin (4)
    GLuint spec_multisample_texture;        // ambient specular (7; 5/6 are the OIT FBO's)
    GLuint depth_renderbuffer;
    // Refraction source: mid-frame resolve of the opaque scene into a mipped RGBA16F texture,
    // created lazily on the first transmissive frame
    GLuint opaque_color_fbo;
    GLuint opaque_color_texture;
    int opaque_color_w, opaque_color_h;
    // Soft-particle source: mid-frame resolve of the scene depth into a single-sample depth
    // texture, created lazily on the first scene_view_resolve_scene_depth call.
    GLuint scene_depth_fbo;
    GLuint scene_depth_texture;
    int scene_depth_w, scene_depth_h;
    // Weighted-blended OIT: a lazily-created FBO sharing depth_renderbuffer (spec 4.17).
    GLuint oit_fbo;
    GLuint oit_accum_multisample_texture;
    GLuint oit_revealage_multisample_texture;
    int oit_w, oit_h;
    // Moment-based OIT (spec 11.17): the generation sub-pass's FBO, and the atlas the accumulate
    // reads its moments from (twice the render height: b1..b4 below, b0 above).
    //
    // They land in one ATLAS rather than staying two textures because the accumulate reads them
    // from pbr_frag, which already declares all sixteen fragment samplers the driver allows -- and
    // the driver counts DECLARED samplers, not distinct units, so there is no seventeenth even for
    // a unit that is provably free. The moments have to arrive through the one sampler that is
    // idle in that sub-pass, which is four floats against their five.
    GLuint moment_fbo;
    GLuint moment_multisample_texture;    // attachment 5: b1..b4
    GLuint moment_b0_multisample_texture; // attachment 6: b0 (.r)
    GLuint moment_atlas_fbo;
    GLuint moment_atlas_texture;
    int moment_w, moment_h;

    // The camera's matrices this frame, derived after the pre-render hook.
    mat4 view_matrix;
    mat4 projection_matrix; // un-jittered: frustum culling, motion vectors
    mat4 view_proj;         // un-jittered projection * view, computed once per scene pass
    // The projection the scene rasterized with: jittered under TAA, projection_matrix otherwise.
    // A post pass reconstructing positions from depth uses this one: marching with the un-jittered
    // matrix flips marginal hits every jitter phase, and TAA bakes the flicker into crosshatch.
    mat4 draw_projection;
    mat4 prev_view_proj; // last frame's view_proj, for motion vectors
    // Where the camera was last frame, in storage space, for the CDLOD morph's previous position,
    // and whether there was a last frame: without it frame 0 reports the camera as having
    // travelled from the world origin.
    vec3 prev_camera_position;
    bool prev_camera_valid;
    // How far the camera moved over the frame just drawn, in storage space: zero on the first
    // frame. An origin shift moves both ends of it, so it is never counted as travel.
    vec3 camera_travel;

    // The mode this frame renders with: the settings', or albedo under the wireframe overlay.
    RenderMode render_mode;
    // Which G-buffer targets this frame's scene pass writes, settled before it runs.
    bool normals_this_frame;
    bool aux_this_frame;
    bool albedo_this_frame;
    bool sss_this_frame;
    bool spec_this_frame;
    bool oit_this_frame;         // the OIT accumulate ran; the chain composites oit_fbo
    bool moments_this_frame;     // the accumulate was weighted by measured transmittance
    bool scene_color_this_frame; // the refraction resolve ran; transmissive draws may sample it
    bool taa_jitter_this_frame;  // the TAA accumulator is live and the projection is jittered

    PostFX* postfx;    // the view's chain; its settings are the view's. NULL for a capture's
    Exposure exposure; // the view's meter; its settings are the view's
    CloudMarch clouds; // the cloud march's targets and history, from this view's camera

    // The view's own frame count: what every history's parity and adjacency are kept by, so a
    // view that rests reads the side it last wrote. Starts at the engine's frame count when the
    // view is made and counts the frames it draws, so a view drawn every frame keeps the engine's.
    long long draw_index;

    // What the view showed last frame, compared byte for byte with what it shows this frame:
    // anything different, or anything that animates forever, is a change (spec 14.38).
    struct {
        bool valid;
        const struct Scene* scene; // the scene shown
        SceneViewSettings settings;
        mat4 view;
        mat4 projection;
        int rect[4];
        uint32_t mask;
        uint64_t changes; // the scene's and the engine's change counts, summed
        uint64_t epoch;   // the graph's
        uint64_t chain;   // the chain's passes and grade, digested
        bool covered;     // a loading screen covered the frame
        bool through;     // the window drawn through its picture, which sets the picture's format
        const struct SceneTags* tags; // the table borrowed, and its generation
        uint64_t tag_generation;
    } record;
    // The block the view's class rows, fade and table count go up in (spec 14.39), and what it
    // holds (`tag_block_held` once it holds anything).
    struct Ubo* tag_block;
    uint32_t tag_block_data[UBO_TAG_VIEW_BLOCK_SIZE / 4];
    bool tag_block_held;
    // This frame draws its see-through surfaces through the OIT passes: it ghosts, and OIT can
    // run. Otherwise those surfaces stay in the opaque passes.
    bool ghosts_this_frame;
    // Its outlines (spec 14.39): the mask of outlined surfaces, with the depth of its solid ones,
    // and the ring drawn from it, which the copy into the window lays over the picture; the
    // table's outline writes the ring was drawn under.
    GLColorTarget outline_mask;
    GLColorTarget overlay;
    bool overlay_valid;
    uint64_t outline_generation;
    // This frame, as _engine_decide_view settled it: whether the view draws, and when it draws an
    // averaged frame, which (average_n, from 0; -1 = a TAA frame).
    bool draws_this_frame;
    int average_n;
    // The view could not draw on a frame since it last drew -- hidden, sized to nothing, its
    // targets refused -- and whether a change it shows has not been drawn by it: what changed while
    // it was away, or what the world's settle delivered on a frame it drew no TAA frame. Its next
    // frame draws it, a cut if it was away.
    bool away;
    bool missed;
    bool taau_said; // accumulation below render scale 1 was said once
    // The last TAA picture, shown while the average holds fewer than VIEW_AVERAGE_SHOWN frames.
    GLColorTarget held;
    bool held_valid;
    // An exact background's working value this frame (background_working), and what has been said
    // once: a colour the curve cannot reach, a stage the inverse cannot undo.
    vec3 exact_background;
    bool exact_unreachable_said;
    bool exact_stage_said;
} SceneViewState;

// How many frames the average holds before it is shown in place of the last TAA picture.
#define VIEW_AVERAGE_SHOWN 4
// The frames an average may be asked to hold: fewer leaves the jitter's pattern in it, and more
// than the sequence's 64 phases would repeat them.
#define VIEW_ACCUMULATE_MIN 16
#define VIEW_ACCUMULATE_MAX 64

// A view for the captures: no target or chain, its camera placed by each face.
struct SceneView* create_capture_view(struct Engine* engine);

// Whether the view draws with tags (spec 14.39): a table, a class row, a fade or the tags render
// mode. A capture never does.
bool scene_view_draws_tags(const struct SceneView* view);
// Whether it draws a surface see-through: a fade, or a class row less than solid. Its opaque items
// are then drawn through the transparent passes too.
bool scene_view_ghosts(const struct SceneView* view);
// Whether it draws any surface otherwise than as it is solid: hidden by a tag or a class, or
// see-through.
bool scene_view_hides_by_tags(const struct SceneView* view);
// `view`'s tags bound for a pass drawing the scene: its block on UBO_BINDING_TAG_VIEW, up to date,
// and its table on TEXUNIT_TAGS. NULL, a view drawing no tags or a capture's binds the engine's
// block of zeros, which draws every surface as it is.
void engine_bind_tags(const struct Engine* engine, const struct SceneView* view);

// The window's framebuffer pixels to a point, across and down: 1 where the window has no size.
void engine_fb_per_point(const struct Engine* engine, float* sx, float* sy);
// The view's rectangle in framebuffer pixels, GL's origin (bottom-left): x, y, width, height. Each
// edge rounds to the nearest pixel, so two views sharing an edge share a pixel boundary.
void scene_view_fb_rect(const struct SceneView* view, int rect[4]);
// The view's size in framebuffer pixels, its post size (that times its supersampling), and the
// render size its targets were last built at.
void scene_view_out_size(const struct SceneView* view, int* w, int* h);
void scene_view_post_size(const struct SceneView* view, int* w, int* h);
void scene_view_render_size(const struct SceneView* view, int* w, int* h);

// Build the view's scene target and chain at the sizes its settings and the window ask for, or
// rebuild them when those moved. False when the view cannot be drawn: a zero area, or a rebuild
// that failed, which is retried only at a different size.
bool scene_view_ensure_targets(struct SceneView* view);

// Clear the scene target for a frame: colour to the background, depth and stencil, and each
// G-buffer target this frame writes to zero.
void scene_view_clear(const struct SceneView* view);
// The draw-buffer list: colour alone, or colour and every G-buffer target this frame writes. A
// capture's target has colour alone.
void scene_view_set_draw_buffers(const struct SceneView* view, bool with_gbuffer);

// The mid-frame resolves and the OIT sub-passes, each leaving the view's framebuffer bound.
bool scene_view_resolve_opaque_color(struct SceneView* view);
GLuint scene_view_resolve_scene_depth(struct SceneView* view);
bool scene_view_begin_oit_pass(struct SceneView* view);
void scene_view_end_oit_pass(struct SceneView* view);
// Whether the accumulate's targets are made at the view's render size, made if not; the
// framebuffer bound is left as it was.
bool scene_view_oit_ready(struct SceneView* view);
bool scene_view_begin_moment_pass(struct SceneView* view);
void scene_view_end_moment_pass(struct SceneView* view);

#endif // _SCENE_VIEW_STATE_H_
