#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "scene_view.h"
#include "scene_view_state.h"
#include "camera_rig.h"
#include "engine.h"
#include "intersect.h"
#include "scene_tags_internal.h"
#include "ubo.h"
#include "util.h"

#include "ext/log.h"

// One row per scene-MRT color attachment, so the per-frame clear, the draw-buffer list and
// create/destroy read one description of what each target is.
//
// Attachment 0 is HDR scene color: always written, and cleared with the view's background by the
// shared glClear (which also clears depth/stencil), so its write gate is NULL and the 1..N clear
// loop skips it.
//
// Each row names its color attachment explicitly rather than deriving it from the row index:
// fragment-output locations 5 and 6 belong to the OIT accumulate targets, which live on the OIT
// FBO -- so the ambient-specular row binds attachment 7 and the scene FBO simply has no
// attachments 5/6.
typedef struct GBufferAttachment {
    GLuint* tex;            // storage on the view's state
    bool* this_frame;       // per-frame write gate (NULL = always written)
    GLenum internal_format; // multisample texture format
    GLenum attachment;      // GL_COLOR_ATTACHMENTn == fragment output location n
    GLfloat clear[4];       // per-frame clear color
} GBufferAttachment;

#define GBUFFER_ATTACHMENT_COUNT 6
// Draw-buffer slots the scene MRT spans: highest attachment in the table (7) + 1. Sizes the
// draw-buffer list and the GL_MAX_DRAW_BUFFERS check, so a table row past slot 7 fails the
// startup check instead of silently overrunning the list.
#define GBUFFER_DRAW_BUFFER_SLOTS 8

// Describe a view's scene-MRT attachments. Pure (no GL): the tex/this_frame pointers alias the
// state's fields so create/destroy/clear share one description of what each attachment is. The
// aux buffer is full float (RGBA32F): fp16 quantizes linear Z into scene-scale steps on large
// scenes and GTAO reads the staircase as banded occlusion. Ambient specular is R11G11B10F:
// positive HDR only (WS_SCENE_MAX sits under the format's ~65024 ceiling), no alpha needed, half
// the bandwidth of RGBA16F.
static void _gbuffer_attachments(const SceneView* view,
                                 GBufferAttachment out[GBUFFER_ATTACHMENT_COUNT]) {
    SceneViewState* vs = view->state;
    const float* bg =
        view->settings.background_exact ? vs->exact_background : view->settings.background;
    out[0] = (GBufferAttachment){&vs->multisample_texture,
                                 NULL,
                                 GL_RGBA16F,
                                 GL_COLOR_ATTACHMENT0,
                                 {bg[0], bg[1], bg[2], 1.0f}};
    out[1] = (GBufferAttachment){&vs->normal_multisample_texture,
                                 &vs->normals_this_frame,
                                 GL_RGBA16F,
                                 GL_COLOR_ATTACHMENT1,
                                 {0.0f, 0.0f, 0.0f, 0.0f}};
    out[2] = (GBufferAttachment){&vs->aux_multisample_texture,
                                 &vs->aux_this_frame,
                                 GL_RGBA32F,
                                 GL_COLOR_ATTACHMENT2,
                                 {0.0f, 0.0f, 0.0f, 0.0f}};
    out[3] = (GBufferAttachment){&vs->albedo_multisample_texture,
                                 &vs->albedo_this_frame,
                                 GL_RGBA8,
                                 GL_COLOR_ATTACHMENT3,
                                 {0.0f, 0.0f, 0.0f, 0.0f}};
    out[4] = (GBufferAttachment){&vs->sss_diffuse_multisample_texture,
                                 &vs->sss_this_frame,
                                 GL_RGBA16F,
                                 GL_COLOR_ATTACHMENT4,
                                 {0.0f, 0.0f, 0.0f, 0.0f}};
    out[5] = (GBufferAttachment){&vs->spec_multisample_texture,
                                 &vs->spec_this_frame,
                                 GL_R11F_G11F_B10F,
                                 GL_COLOR_ATTACHMENT7,
                                 {0.0f, 0.0f, 0.0f, 0.0f}};
}

static void _free_targets(SceneView* view);

/*
 * Settings and lifetime
 */

void engine_init_view_settings(const Engine* engine, SceneViewSettings* s) {
    if (!s)
        return;
    memset(s, 0, sizeof(*s));
    postfx_settings_init(&s->post);
    postfx_settings_reset_lengths(&s->post, engine ? engine->typical_size : 1.0f);
    exposure_settings_init(&s->exposure);
    s->samples = 4; // 4x MSAA by default
    s->ss_scale = 1;
    s->render_scale = 1.0f;
    glm_vec3_copy((vec3){0.1f, 0.1f, 0.1f}, s->background);
    s->render_mode = RENDER_MODE_PBR;
    // The canvas's ring width; fainter by about a third where what it rings is hidden.
    s->outline_points = 1.5f;
    s->outline_hidden = 0.35f;
}

// The view and its state, nothing built.
static SceneView* _scene_view_alloc(Engine* engine, const SceneViewDesc* desc) {
    static const SceneViewDesc none = {.fills_window = true};
    if (!desc)
        desc = &none;
    SceneView* view = calloc(1, sizeof(SceneView));
    SceneViewState* vs = calloc(1, sizeof(SceneViewState));
    if (!view || !vs) {
        log_error("Failed to allocate memory for a scene view");
        free(view);
        free(vs);
        return NULL;
    }
    camera_init(&view->camera, &desc->camera);
    if (desc->settings)
        view->settings = *desc->settings;
    else
        engine_init_view_settings(engine, &view->settings);
    view->fills_window = desc->fills_window;
    view->x = desc->x;
    view->y = desc->y;
    view->width = desc->width;
    view->height = desc->height;
    view->mask = desc->mask ? desc->mask : ~0u;
    view->state = vs;
    vs->engine = engine;
    vs->draw_index = (long long)engine->total_frames;
    vs->average_n = -1;
    vs->render_mode = view->settings.render_mode;
    exposure_init(&vs->exposure, &view->settings.exposure);
    cloud_march_init(&vs->clouds);
    glm_mat4_identity(vs->view_matrix);
    glm_mat4_identity(vs->projection_matrix);
    glm_mat4_identity(vs->view_proj);
    glm_mat4_identity(vs->draw_projection);
    glm_mat4_identity(vs->prev_view_proj);
    return view;
}

SceneView* create_scene_view(Engine* engine, const SceneViewDesc* desc) {
    if (!engine) {
        log_error("create_scene_view: NULL engine");
        return NULL;
    }
    SceneView* view = _scene_view_alloc(engine, desc);
    if (!view)
        return NULL;
    // Built now, so the chain exists for a grade or a pass the app adds before the first frame. A
    // window with no area yet (minimized at creation) builds at its first frame instead; only a
    // build that was tried and failed refuses the view.
    if (!scene_view_ensure_targets(view) && view->state->render_suspended) {
        log_error("create_scene_view: the view's buffers could not be made");
        free_scene_view(view);
        return NULL;
    }
    return view;
}

SceneView* create_capture_view(Engine* engine) {
    SceneView* view = _scene_view_alloc(engine, NULL);
    if (view)
        view->state->capturing = true;
    return view;
}

_Static_assert(TAG_VIEW_CLASSES == SCENE_TAG_CLASSES,
               "the view's tag block holds a row for each of its classes");

static uint32_t _unorm16(float v) {
    return (uint32_t)lroundf(glm_clamp(v, 0.0f, 1.0f) * 65535.0f);
}

// The block is built in one pass over the rows, as tags.glsl reads it: its head -- the flags, the
// table's count, the fade -- then a row a class, the colour, see-through and flat in 16 bits each,
// and whether it is hidden. Class 0 is none and its row left zero. The flags are what the rest of
// the frame asks of the view, so it asks one answer.
void scene_view_latch_tags(SceneView* view) {
    SceneViewState* vs = view->state;
    const SceneViewSettings* s = &view->settings;
    uint32_t block[UBO_TAG_VIEW_BLOCK_SIZE / 4] = {0};
    bool rows = false, ghosts = s->tag_fade > 0.0f, drops = false;
    for (int c = 1; c < TAG_VIEW_CLASSES; c++) {
        const SceneTagClass* k = &s->tag_classes[c];
        uint32_t* row = &block[4 + 4 * c];
        row[0] = k->colour;
        row[1] = _unorm16(k->see_through) | (_unorm16(k->flat) << 16);
        row[2] = k->hidden ? 1u : 0u;
        rows = rows || row[0] != 0 || row[1] != 0 || row[2] != 0;
        ghosts = ghosts || ((row[1] & 0xFFFFu) != 0 && !k->hidden);
        drops = drops || k->hidden;
    }
    const bool on =
        !vs->capturing && vs->engine->tags_supported &&
        (view->tags || s->tag_fade > 0.0f || s->render_mode == RENDER_MODE_TAGS || rows);
    vs->tag_outline_generation = scene_tags_outline_generation(on ? view->tags : NULL);
    if (on && !vs->tag_block)
        vs->tag_block = create_ubo(UBO_TAG_VIEW_BLOCK_SIZE, UBO_BINDING_TAG_VIEW);
    if (!on || !vs->tag_block) {
        memset(vs->tag_block_data, 0, sizeof(vs->tag_block_data));
        return;
    }
    const float fade = glm_clamp(s->tag_fade, 0.0f, 1.0f);
    drops = drops || ghosts || scene_tags_hides(view->tags);
    block[0] = TAG_VIEW_ON | (drops ? TAG_VIEW_DROPS : 0u) | (ghosts ? TAG_VIEW_GHOSTS : 0u) |
               (scene_tags_outlined(view->tags) ? TAG_VIEW_OUTLINED : 0u);
    block[1] = scene_tags_upload(view->tags);
    memcpy(&block[2], &fade, sizeof(fade));
    // Held as the block holds it, which a block never uploaded cannot match: its head is never 0.
    if (memcmp(block, vs->tag_block_data, sizeof(block)) != 0) {
        ubo_upload(vs->tag_block, block, sizeof(block));
        memcpy(vs->tag_block_data, block, sizeof(block));
    }
}

void engine_bind_tags(const Engine* engine, const SceneView* view) {
    const bool own = scene_view_draws_tags(view);
    if (own)
        scene_tags_bind(view->tags, TEXUNIT_TAGS);
    glBindBufferBase(GL_UNIFORM_BUFFER, UBO_BINDING_TAG_VIEW,
                     own ? view->state->tag_block->id : engine->tag_block_none->id);
}

void free_scene_view(SceneView* view) {
    if (!view)
        return;
    SceneViewState* vs = view->state;
    if (vs->added && !engine_remove_view(vs->engine, view))
        return;
    _free_targets(view);
    cloud_march_release(&vs->clouds);
    free_ubo(vs->tag_block);
    gl_color_target_free(&vs->outline_mask);
    gl_color_target_free(&vs->overlay);
    free(vs);
    camera_release(&view->camera);
    free(view);
}

bool engine_add_view(Engine* engine, SceneView* view) {
    if (!engine || !view) {
        log_error("engine_add_view: NULL engine or view");
        return false;
    }
    if (view->state->engine != engine || view->state->capturing) {
        log_error("engine_add_view: the view was made by another engine");
        return false;
    }
    if (view->state->added) {
        log_error("engine_add_view: the view is added already");
        return false;
    }
    if (!grow_array((void**)&engine->views, &engine->view_capacity, engine->view_count + 1,
                    sizeof(SceneView*), 4)) {
        log_error("engine_add_view: out of memory");
        return false;
    }
    engine->views[engine->view_count++] = view;
    view->state->added = true;
    // A view added again has missed whatever changed while it was out: its next frame is a cut.
    view->state->away = view->state->record.valid;
    view->state->missed = view->state->record.valid;
    if (!engine->focus_view)
        engine->focus_view = view;
    if (view->rig)
        view->rig->typical_size = engine->typical_size;
    return true;
}

SceneView* engine_remove_view(Engine* engine, SceneView* view) {
    if (!engine || !view)
        return NULL;
    if (engine->views_drawing) {
        log_error("engine_remove_view: refused while the frame draws its views");
        return NULL;
    }
    for (size_t i = 0; i < engine->view_count; i++) {
        if (engine->views[i] != view)
            continue;
        memmove(&engine->views[i], &engine->views[i + 1],
                (engine->view_count - i - 1) * sizeof(SceneView*));
        engine->view_count--;
        view->state->added = false;
        if (engine->focus_view == view)
            engine->focus_view = engine->view_count > 0 ? engine->views[0] : NULL;
        return view;
    }
    return NULL;
}

void engine_set_focus_view(Engine* engine, SceneView* view) {
    if (!engine || !view)
        return;
    if (!view->state->added || view->state->engine != engine) {
        log_error("engine_set_focus_view: the view is not one of this engine's");
        return;
    }
    engine->focus_view = view;
}

void scene_view_set_rig(SceneView* view, struct CameraRig* rig) {
    if (!view)
        return;
    if (view->rig && rig && view->rig != rig)
        log_info("camera rig replaced; the previous one no longer moves the camera");
    view->rig = rig;
    if (rig)
        rig->typical_size = view->state->engine->typical_size;
}

void scene_view_set_2d_preset(SceneView* view) {
    if (!view)
        return;
    PostFXSettings* p = &view->settings.post;
    p->bloom_enabled = false;
    p->glare_enabled = false;
    p->local_exposure_enabled = false;
    p->ssao_enabled = false;
    p->ssr_enabled = false;
    p->vignette_enabled = false;
    p->dither_enabled = false;
    p->taa_enabled = false;
    p->tonemap_mode = POSTFX_TONEMAP_LINEAR;
    view->settings.exposure.physical = false;
    view->settings.exposure.automatic = false;
    view->settings.exposure.multiplier = 1.0f;
}

bool scene_view_load_lut(SceneView* view, const char* path) {
    if (!view || !view->state->postfx) {
        log_error("scene_view_load_lut: no view, or one with no post chain");
        return false;
    }
    return postfx_load_lut(view->state->postfx, path);
}

void scene_view_clear_lut(SceneView* view) {
    if (view && view->state->postfx)
        postfx_clear_lut(view->state->postfx);
}

PostFXPass* scene_view_add_pass(SceneView* view, PostFXLocation at, ShaderProgram* program) {
    if (!view || !view->state->postfx) {
        log_error("scene_view_add_pass: no view, or one with no post chain");
        return NULL;
    }
    return postfx_add_pass(view->state->postfx, at, program);
}

/*
 * Points and rays
 */

// Framebuffer pixels per window point, along each axis.
void engine_fb_per_point(const Engine* engine, float* sx, float* sy) {
    *sx = engine->win_width > 0 ? (float)engine->fb_width / (float)engine->win_width : 1.0f;
    *sy = engine->win_height > 0 ? (float)engine->fb_height / (float)engine->win_height : 1.0f;
}

void scene_view_fb_rect(const SceneView* view, int rect[4]) {
    const Engine* engine = view->state->engine;
    if (view->fills_window) {
        rect[0] = 0;
        rect[1] = 0;
        rect[2] = engine->fb_width;
        rect[3] = engine->fb_height;
        return;
    }
    float sx, sy;
    engine_fb_per_point(engine, &sx, &sy);
    const int left = (int)lroundf(view->x * sx);
    const int right = (int)lroundf((view->x + view->width) * sx);
    const int top = (int)lroundf(view->y * sy);
    const int bottom = (int)lroundf((view->y + view->height) * sy);
    rect[0] = left;
    rect[1] = engine->fb_height - bottom;
    rect[2] = right > left ? right - left : 0;
    rect[3] = bottom > top ? bottom - top : 0;
}

void scene_view_point_from_fb(const SceneView* view, double fb_x, double fb_y, float out[2]) {
    const Engine* engine = view->state->engine;
    float sx, sy;
    engine_fb_per_point(engine, &sx, &sy);
    int rect[4];
    scene_view_fb_rect(view, rect);
    // fb_y counts up from the window's bottom edge, as the rect's origin does; the point counts
    // down from the view's top.
    out[0] = (float)(fb_x - rect[0]) / sx;
    out[1] = (float)(rect[3] - (fb_y - rect[1])) / sy;
}

bool scene_view_contains(const SceneView* view, const float point[2]) {
    const Engine* engine = view->state->engine;
    float sx, sy;
    engine_fb_per_point(engine, &sx, &sy);
    int rect[4];
    scene_view_fb_rect(view, rect);
    return point[0] >= 0.0f && point[1] >= 0.0f && point[0] * sx < (float)rect[2] &&
           point[1] * sy < (float)rect[3];
}

void scene_view_ray(const SceneView* view, const float point[2], vec3 origin, vec3 dir) {
    glm_vec3_zero(origin);
    glm_vec3_copy((vec3){0.0f, 0.0f, -1.0f}, dir);
    if (!view)
        return;
    const Engine* engine = view->state->engine;
    float sx, sy;
    engine_fb_per_point(engine, &sx, &sy);
    int rect[4];
    scene_view_fb_rect(view, rect);
    const Camera* camera = &view->camera;
    mat4 projection = {0}, view_m;
    camera_projection_matrix(camera, projection);
    glm_lookat((float*)camera->position, (float*)camera->look_at, (float*)camera->up_vector,
               view_m);
    // In the view's own framebuffer pixels, +Y up, as the projection's NDC runs. Under ortho the
    // ray starts at the pixel rather than at the camera, so the origin has to travel with the
    // direction.
    const float px = point[0] * sx, py = (float)rect[3] - point[1] * sy;
    if (camera->is_orthographic) {
        compute_ortho_ray_from_screen(px, py, rect[2], rect[3], projection, view_m, origin, dir);
    } else {
        glm_vec3_copy((float*)camera->position, origin);
        compute_ray_from_screen(px, py, rect[2], rect[3], projection, view_m, origin, dir);
    }
}

void scene_view_drag_point(const SceneView* view, const float point[2], vec3 out_world_pos) {
    if (!view)
        return;
    vec3 origin = {0.0f, 0.0f, 0.0f}, dir = {0.0f, 0.0f, -1.0f};
    scene_view_ray(view, point, origin, dir);
    ray_point_at_distance(origin, dir, view->state->engine->input.drag_plane_distance,
                          out_world_pos);
}

/*
 * Sizes and targets
 */

void scene_view_out_size(const SceneView* view, int* w, int* h) {
    int rect[4];
    scene_view_fb_rect(view, rect);
    *w = rect[2];
    *h = rect[3];
}

// The supersampling a view's settings ask for, as the chain can run it: the tone-map downsample is
// an exact box filter only at 2x (GL linear sampling averages one 2x2 texel block); higher factors
// under-resolve and can also exceed GL_MAX_TEXTURE_SIZE.
static int _view_ss_scale(const SceneView* view) {
    const int ss = view->settings.ss_scale;
    return ss < 1 ? 1 : (ss > 2 ? 2 : ss);
}

// The render scale a view's settings ask for, as the chain can run it. TAAU reconstructs from the
// jitter, and headless suppresses the jitter unless headless_jitter is set -- without it the
// resolve integrates one repeated sample position forever, which does not fail, it just looks
// permanently soft -- so a headless view without it runs at full resolution.
static float _view_render_scale(const SceneView* view) {
    float scale = postfx_clamp_render_scale(view->settings.render_scale);
    if (view->state->engine->headless && !view->settings.headless_jitter && scale < 1.0f)
        scale = 1.0f;
    return scale;
}

void scene_view_post_size(const SceneView* view, int* w, int* h) {
    scene_view_out_size(view, w, h);
    const int ss = _view_ss_scale(view);
    *w *= ss;
    *h *= ss;
}

void scene_view_render_size(const SceneView* view, int* w, int* h) {
    *w = view->state->target_render_w;
    *h = view->state->target_render_h;
}

// Clamp a requested MSAA sample count to [1, driver max]. Needs a live GL context.
static int _clamp_msaa_samples(int samples) {
    if (samples < 1)
        samples = 1;
    GLint max_color_samples = 0;
    GLint max_samples = 0;
    glGetIntegerv(GL_MAX_COLOR_TEXTURE_SAMPLES, &max_color_samples);
    glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    if (max_samples > 0 && max_samples < max_color_samples)
        max_color_samples = max_samples;
    if (max_color_samples > 0 && samples > max_color_samples)
        samples = max_color_samples;
    return samples;
}

// Delete the multisample color/normal attachments and depth renderbuffer.
// The framebuffer object itself is left intact for reuse.
static void _destroy_msaa_attachments(SceneView* view) {
    SceneViewState* vs = view->state;
    GBufferAttachment gb[GBUFFER_ATTACHMENT_COUNT];
    _gbuffer_attachments(view, gb);
    for (int i = 0; i < GBUFFER_ATTACHMENT_COUNT; i++) {
        gl_delete_texture(gb[i].tex);
    }
    glDeleteRenderbuffers(1, &vs->depth_renderbuffer);
    vs->depth_renderbuffer = 0;
    // The OIT and moment FBOs share depth_renderbuffer; tear them down too so
    // they are rebuilt against the fresh depth on the next OIT frame (0 handles
    // when never created).
    gl_delete_fbo(&vs->oit_fbo);
    gl_delete_texture(&vs->oit_accum_multisample_texture);
    gl_delete_texture(&vs->oit_revealage_multisample_texture);
    vs->oit_w = 0;
    vs->oit_h = 0;
    gl_delete_fbo(&vs->moment_fbo);
    gl_delete_texture(&vs->moment_multisample_texture);
    gl_delete_texture(&vs->moment_b0_multisample_texture);
    gl_delete_fbo(&vs->moment_atlas_fbo);
    gl_delete_texture(&vs->moment_atlas_texture);
    vs->moment_w = 0;
    vs->moment_h = 0;
}

// Create one scene color attachment (texture + framebuffer binding) on the
// currently-bound framebuffer -- multisample above one sample, a plain
// GL_TEXTURE_2D at one. Each G-buffer target is written only when a consumer is
// active (see scene_view_set_draw_buffers); the texture is always allocated so
// the FBO layout is stable across frames.
//
// The branch is HERE, in the one allocator, rather than at its four call sites,
// and that placement is load-bearing: the scene, OIT and moment FBOs all share
// depth_renderbuffer, and GL requires every attachment of an FBO to carry the
// same sample count. One branch on the one field every caller passes flips all
// three FBOs in lockstep by construction; four caller-side branches could
// disagree and the failure would be a quietly-incomplete OIT FBO falling back
// to the unsorted late pass.
//
// It used to be multisample even at samples == 1, which was a live cost: this
// driver refuses a 1-sample multisample target and returns a 2-sample one (see
// msaa_samples_actual), so the TAA path -- which requests one sample precisely
// to avoid MSAA -- silently rasterized two samples per pixel (spec 11.34).
//
// The single-sample texture sets sampler state where the multisample one cannot
// carry any: nothing samples these targets today (every consumer is a blit),
// but a plain texture left at the GL_NEAREST_MIPMAP_LINEAR default would be
// mip-incomplete the day something does, and that failure is black, not loud.
static void _add_scene_color_attachment(GLuint* out_tex, GLenum internal_format, GLenum attachment,
                                        int rw, int rh, int samples) {
    glGenTextures(1, out_tex);
    if (samples > 1) {
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, *out_tex);
        glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, internal_format, rw, rh,
                                GL_TRUE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D_MULTISAMPLE, *out_tex, 0);
        return;
    }
    glBindTexture(GL_TEXTURE_2D, *out_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal_format, rw, rh, 0,
                 gl_transfer_format(internal_format), GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, *out_tex, 0);
}

// (Re)create the multisample attachments on the view's framebuffer at the given
// sample count and render size. Attachment 0 is float (RGBA16F) so the scene
// accumulates in linear HDR; the G-buffer targets follow.
static int _create_msaa_attachments(SceneView* view, int rw, int rh, int samples) {
    SceneViewState* vs = view->state;
    glBindFramebuffer(GL_FRAMEBUFFER, vs->framebuffer);

    // GL 4.1 guarantees >= 8 draw buffers, but query once so a hypothetical
    // thin driver fails loudly here instead of silently dropping the last
    // attachment.
    GBufferAttachment gb[GBUFFER_ATTACHMENT_COUNT];
    _gbuffer_attachments(view, gb);
    GLint max_draw_buffers = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &max_draw_buffers);
    if (max_draw_buffers < GBUFFER_DRAW_BUFFER_SLOTS)
        log_error("GL_MAX_DRAW_BUFFERS is %d (< %d): the last scene-MRT attachment will not bind",
                  max_draw_buffers, GBUFFER_DRAW_BUFFER_SLOTS);

    // Draw-buffer layout: 0 HDR color, 1 view normals (SSAO/SSR), 2 aux (TAA
    // motion + GTAO linear-Z), 3 albedo (SSGI), 4 skin diffuse (SSS), 7
    // ambient specular (split spec-occ). Every target is allocated each
    // (re)build for a stable FBO layout, but written only when its consumer is
    // active (see scene_view_set_draw_buffers).
    for (int i = 0; i < GBUFFER_ATTACHMENT_COUNT; i++) {
        _add_scene_color_attachment(gb[i].tex, gb[i].internal_format, gb[i].attachment, rw, rh,
                                    samples);
    }

    glGenRenderbuffers(1, &vs->depth_renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, vs->depth_renderbuffer);
    // Same branch as the colour allocator, for the same shared-depth reason: a
    // plain renderbuffer at one sample, multisample above. It stays a
    // renderbuffer either way -- it was never sampleable, which is why
    // scene_view_resolve_scene_depth exists.
    if (samples > 1)
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, rw, rh);
    else
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, rw, rh);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                              vs->depth_renderbuffer);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("Error: MSAA Framebuffer is not complete!");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return -1;
    }

    // What the driver ACTUALLY gave us, which above one sample is not always
    // what was asked for. Read back while the target is still bound, once per
    // build, so no consumer has to repeat the query or -- worse -- trust the
    // request (spec 11.31, 11.34).
    GLint got = 0;
    glGetIntegerv(GL_SAMPLES, &got);
    vs->msaa_samples_actual = got < 1 ? 1 : got;
    if (vs->msaa_samples_actual != samples)
        log_info("Scene target: asked for %dx MSAA, driver gave %dx", samples,
                 vs->msaa_samples_actual);

    glBindFramebuffer(GL_FRAMEBUFFER, 0); // Bind back to the default framebuffer
    return 0;
}

// The scene target at the given size and the view's sample count, on the same FBO name. The one
// place that pairing lives, so a caller cannot destroy without recreating, or ignore the result --
// an incomplete scene FBO is invisible until something draws into it.
static bool _rebuild_msaa_target(SceneView* view, int rw, int rh) {
    SceneViewState* vs = view->state;
    _destroy_msaa_attachments(view);
    if (_create_msaa_attachments(view, rw, rh, vs->msaa_samples_built) != 0) {
        log_error("Scene target rebuild failed at %dx%d (%dx MSAA)", rw, rh,
                  vs->msaa_samples_built);
        return false;
    }
    return true;
}

bool scene_view_ensure_targets(SceneView* view) {
    SceneViewState* vs = view->state;
    Engine* engine = vs->engine;
    if (vs->capturing)
        return false;
    scene_view_fb_rect(view, vs->picture_rect);
    int ow, oh, pw, ph;
    scene_view_out_size(view, &ow, &oh);
    scene_view_post_size(view, &pw, &ph);
    const int ss = _view_ss_scale(view);
    const float scale = _view_render_scale(view);
    const int rw = postfx_scaled_dim(pw, scale);
    const int rh = postfx_scaled_dim(ph, scale);
    // A zero-area view (a minimized window) is where every downstream size formula degenerates:
    // NaN aspect, +inf cluster params, incomplete framebuffers. Its targets are held untouched
    // and it is skipped, so nothing has to be un-set when it returns.
    if (rw < 1 || rh < 1 || pw < 1 || ph < 1)
        return false;

    // Keyed on the built sizes rather than on an event, so that every trigger -- the GUI slider, a
    // window resize, the diagnostic schedule, a rectangle moved -- works without its own call
    // site. Already built (or already attempted) at these sizes and this count: nothing to do,
    // which is also the retry policy -- a failed attempt is not repeated until a different size is
    // asked for.
    const bool sized = rw == vs->target_render_w && rh == vs->target_render_h &&
                       pw == vs->target_post_w && ph == vs->target_post_h &&
                       ow == vs->target_out_w && oh == vs->target_out_h;
    const int samples = _clamp_msaa_samples(view->settings.samples);
    if (sized && samples == vs->msaa_samples_built && (vs->postfx || vs->render_suspended))
        return !vs->render_suspended;

    // The sample count alone: the scene target is rebuilt in place, and the chain's single-sample
    // resolve targets are unaffected by it.
    if (sized && vs->postfx) {
        vs->msaa_samples_built = samples;
        vs->render_suspended = !_rebuild_msaa_target(view, rw, rh);
        return !vs->render_suspended;
    }

    // Build, or bring to the sizes asked, the scene target and the whole post chain. The scene
    // target goes first and its failure is checked before the chain is touched: every G-buffer
    // resolve is a multisample blit, which needs identical rects, so a half-applied resize is
    // worse than none. What this attempt is FOR is recorded before it is made, so a failure
    // latches on its own size and is retried only when a genuinely different one is asked for.
    const bool rebuild = vs->postfx != NULL;
    vs->target_render_w = rw;
    vs->target_render_h = rh;
    vs->target_post_w = pw;
    vs->target_post_h = ph;
    vs->target_out_w = ow;
    vs->target_out_h = oh;
    vs->msaa_samples_built = samples;
    if (!vs->framebuffer)
        glGenFramebuffers(1, &vs->framebuffer);
    if (!_rebuild_msaa_target(view, rw, rh)) {
        vs->render_suspended = true;
        return false;
    }
    if (rebuild ? !postfx_resize(vs->postfx, ow, oh, ss, scale)
                : !(vs->postfx = create_postfx(engine->postfx_programs, &view->settings.post, ow,
                                               oh, ss, scale))) {
        log_error("A view's post chain could not be made at %dx%d; it draws nothing", rw, rh);
        vs->render_suspended = true;
        return false;
    }
    vs->postfx->exposure = &vs->exposure;
    vs->postfx->profiler = engine->profiler;
    vs->render_suspended = false;
    // The lazily-sized targets (opaque color, scene depth, OIT) each compare their stored size and
    // rebuild themselves on next use.
    if (rebuild)
        log_info("Render targets rebuilt: %dx%d render, scale %.2f", rw, rh, scale);
    return true;
}

static void _free_targets(SceneView* view) {
    SceneViewState* vs = view->state;
    if (vs->postfx) {
        free_postfx(vs->postfx);
        vs->postfx = NULL;
    }
    _destroy_msaa_attachments(view);
    gl_color_target_free(&vs->held);
    vs->held_valid = false;
    gl_delete_fbo(&vs->framebuffer);
    gl_delete_fbo(&vs->opaque_color_fbo);
    gl_delete_texture(&vs->opaque_color_texture);
    gl_delete_fbo(&vs->scene_depth_fbo);
    gl_delete_texture(&vs->scene_depth_texture);
}

/*
 * The scene pass's targets
 */

void scene_view_clear(const SceneView* view) {
    scene_view_set_draw_buffers(view, true);
    GBufferAttachment gb[GBUFFER_ATTACHMENT_COUNT];
    _gbuffer_attachments(view, gb);
    glClearColor(gb[0].clear[0], gb[0].clear[1], gb[0].clear[2], gb[0].clear[3]);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    // Each written G-buffer target carries 0 on uncovered/sky/no-signal
    // texels so its consumer early-outs there (GTAO on linear-Z, SSGI on
    // albedo, SSS on skin diffuse). glClear painted them the scene
    // background above, so re-clear the written ones to 0; the skybox and
    // shadow catcher draw color-only and leave them cleared.
    for (int i = 1; i < GBUFFER_ATTACHMENT_COUNT; i++) {
        if (*gb[i].this_frame)
            glClearBufferfv(GL_COLOR, (GLint)(gb[i].attachment - GL_COLOR_ATTACHMENT0),
                            gb[i].clear);
    }
}

void scene_view_set_draw_buffers(const SceneView* view, bool with_gbuffer) {
    if (!view)
        return;

    // The opaque pass may publish G-buffer attachments 1 (normals), 2 (aux:
    // motion .xy + linear view-Z .z), and 3 (albedo) alongside color; every other
    // pass is color only. Build the draw-buffer list left to right: GL_NONE fills
    // a skipped slot so the fragment-output-location -> attachment mapping is
    // preserved, and count extends to the highest enabled attachment. Blending is
    // enabled globally, so keep each aux target opaque via indexed disable
    // (re-issued at every pass boundary because any blanket glEnable(GL_BLEND)
    // wipes it).
    // Read each attachment's per-frame write gate from the same descriptor table
    // that drives create/clear. Attachment 0 (this_frame NULL) is always written and
    // starts the list.
    //
    // A capture target has attachment 0 and nothing else, so the whole G-buffer
    // is off for the duration -- asked HERE, once, rather than by clearing each
    // this_frame flag at the capture site. That list was hand-maintained and had
    // already fallen behind: attachment 4 (SSS) was added without one, so a
    // re-convergence sweep in an --sss scene emitted a five-attachment draw list
    // against a one-attachment FBO. Gating the loop instead is correct for
    // attachment 4 and for every attachment added after it.
    const bool gbuffer = with_gbuffer && !view->state->capturing;
    GBufferAttachment gb[GBUFFER_ATTACHMENT_COUNT];
    _gbuffer_attachments(view, gb);
    // bufs[slot] carries GL_COLOR_ATTACHMENT0+slot or GL_NONE (0), so
    // fragment output location N always lands on attachment N. Slots 5/6 stay
    // GL_NONE here -- those locations are the OIT accumulate targets, bound on
    // the OIT FBO.
    GLenum bufs[GBUFFER_DRAW_BUFFER_SLOTS] = {GL_COLOR_ATTACHMENT0}; // rest GL_NONE (0)
    int count = 1;
    for (int i = 1; i < GBUFFER_ATTACHMENT_COUNT; i++) {
        if (gbuffer && *gb[i].this_frame) {
            int slot = (int)(gb[i].attachment - GL_COLOR_ATTACHMENT0);
            bufs[slot] = gb[i].attachment;
            glDisablei(GL_BLEND, slot);
            if (slot + 1 > count)
                count = slot + 1;
        }
    }
    glDrawBuffers(count, bufs);
}

// Refraction blurs the resolved scene by sampling coarser mips; the shader
// clamps its LOD to this, so mip generation stops here too (levels past it
// would never be read)
#define OPAQUE_COLOR_MAX_LOD 6

// Lazy allocation for the refraction source (the postfx ensure_* pattern):
// created on first transmissive frame at the internal render size and
// recreated if that ever changes. Leaves the new FBO bound on success.
static bool _ensure_opaque_color_target(SceneViewState* vs, int rw, int rh) {
    if (vs->opaque_color_texture != 0 && vs->opaque_color_w == rw && vs->opaque_color_h == rh) {
        return true;
    }
    if (vs->opaque_color_texture) {
        glDeleteTextures(1, &vs->opaque_color_texture);
        glDeleteFramebuffers(1, &vs->opaque_color_fbo);
    }
    glGenTextures(1, &vs->opaque_color_texture);
    glBindTexture(GL_TEXTURE_2D, vs->opaque_color_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, rw, rh, 0, GL_RGBA, GL_FLOAT, NULL);
    // CLAMP: refraction offsets must not wrap to the far edge; LINEAR
    // mipmap so roughness-scaled textureLod blends between blur levels
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, OPAQUE_COLOR_MAX_LOD);
    glGenFramebuffers(1, &vs->opaque_color_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, vs->opaque_color_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           vs->opaque_color_texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("Opaque-color resolve framebuffer incomplete");
        gl_delete_texture(&vs->opaque_color_texture);
        gl_delete_fbo(&vs->opaque_color_fbo);
        return false;
    }
    vs->opaque_color_w = rw;
    vs->opaque_color_h = rh;
    return true;
}

// Resolve the MSAA color attachment (opaques + skybox) into the mipped
// single-sample texture transmissive surfaces sample for screen-space
// refraction. The copy is what makes sampling legal: reading the scene
// FBO while drawing into it would be a framebuffer feedback loop.
// Exit state: the view's framebuffer bound with its draw-buffer list intact
// (per-FBO state) and the active texture unit restored to 0, so the late
// pass resumes untouched. Only valid while the view's framebuffer holds the
// scene being drawn -- a capture, which draws into a face of its own, has
// no resolve.
bool scene_view_resolve_opaque_color(SceneView* view) {
    SceneViewState* vs = view->state;
    if (vs->capturing)
        return false;
    int rw, rh;
    scene_view_render_size(view, &rw, &rh);
    if (rw <= 0 || rh <= 0)
        return false;
    if (!_ensure_opaque_color_target(vs, rw, rh)) {
        glBindFramebuffer(GL_FRAMEBUFFER, vs->framebuffer);
        return false;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, vs->framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, vs->opaque_color_fbo);
    glBlitFramebuffer(0, 0, rw, rh, 0, 0, rw, rh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vs->opaque_color_texture);
    glGenerateMipmap(GL_TEXTURE_2D); // box mips (to MAX_LEVEL) = the blur chain
    glBindFramebuffer(GL_FRAMEBUFFER, vs->framebuffer);
    return true;
}

// Lazy single-sample depth resolve target (mirrors _ensure_opaque_color_target).
// DEPTH24_STENCIL8 matches the multisample depth renderbuffer so the depth blit
// is format-compatible; sampled as sampler2D it returns window-space depth in .r.
static bool _ensure_scene_depth_target(SceneViewState* vs, int rw, int rh) {
    if (vs->scene_depth_texture != 0 && vs->scene_depth_w == rw && vs->scene_depth_h == rh) {
        return true;
    }
    if (vs->scene_depth_texture) {
        glDeleteTextures(1, &vs->scene_depth_texture);
        glDeleteFramebuffers(1, &vs->scene_depth_fbo);
    }
    glGenTextures(1, &vs->scene_depth_texture);
    glBindTexture(GL_TEXTURE_2D, vs->scene_depth_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, rw, rh, 0, GL_DEPTH_STENCIL,
                 GL_UNSIGNED_INT_24_8, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &vs->scene_depth_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, vs->scene_depth_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                           vs->scene_depth_texture, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("Scene-depth resolve framebuffer incomplete");
        gl_delete_texture(&vs->scene_depth_texture);
        gl_delete_fbo(&vs->scene_depth_fbo);
        return false;
    }
    vs->scene_depth_w = rw;
    vs->scene_depth_h = rh;
    return true;
}

GLuint scene_view_resolve_scene_depth(SceneView* view) {
    if (!view || view->state->capturing)
        return 0;
    SceneViewState* vs = view->state;
    int rw, rh;
    scene_view_render_size(view, &rw, &rh);
    if (rw <= 0 || rh <= 0)
        return 0;
    if (!_ensure_scene_depth_target(vs, rw, rh)) {
        glBindFramebuffer(GL_FRAMEBUFFER, vs->framebuffer);
        return 0;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, vs->framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, vs->scene_depth_fbo);
    glBlitFramebuffer(0, 0, rw, rh, 0, 0, rw, rh, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, vs->framebuffer);
    return vs->scene_depth_texture;
}

// Lazy allocation for the weighted-blended OIT targets: two multisample color
// textures -- accum (RGBA16F, attachment 5) and revealage (R16F, attachment 6) --
// on a dedicated FBO that SHARES the view's depth renderbuffer, so transparent frags
// depth-test against opaque geometry (without writing depth). Same sample count as
// the scene depth or the FBO would be incomplete. Recreated on a size change; torn
// down with the MSAA attachments (they share the depth renderbuffer).
bool scene_view_oit_ready(SceneView* view) {
    SceneViewState* vs = view->state;
    int rw, rh;
    scene_view_render_size(view, &rw, &rh);
    if (vs->oit_fbo != 0 && vs->oit_w == rw && vs->oit_h == rh)
        return true;
    GLint bound = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
    if (vs->oit_fbo) {
        glDeleteTextures(1, &vs->oit_accum_multisample_texture);
        glDeleteTextures(1, &vs->oit_revealage_multisample_texture);
        glDeleteFramebuffers(1, &vs->oit_fbo);
    }
    glGenFramebuffers(1, &vs->oit_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, vs->oit_fbo);
    _add_scene_color_attachment(&vs->oit_accum_multisample_texture, GL_RGBA16F,
                                GL_COLOR_ATTACHMENT5, rw, rh, vs->msaa_samples_built);
    _add_scene_color_attachment(&vs->oit_revealage_multisample_texture, GL_R16F,
                                GL_COLOR_ATTACHMENT6, rw, rh, vs->msaa_samples_built);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                              vs->depth_renderbuffer);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)bound);
    if (!complete) {
        log_error("OIT framebuffer incomplete");
        gl_delete_texture(&vs->oit_accum_multisample_texture);
        gl_delete_texture(&vs->oit_revealage_multisample_texture);
        gl_delete_fbo(&vs->oit_fbo);
        return false;
    }
    vs->oit_w = rw;
    vs->oit_h = rh;
    return true;
}

// Begin the weighted-blended OIT accumulate sub-pass: bind the OIT FBO (accum +
// revealage sharing the scene depth), clear accum to 0 / revealage to 1, and set
// the independent per-target blend (accum sums, revealage multiplies 1-alpha).
// Returns false if the targets couldn't be allocated (caller falls back to the
// classic unsorted late pass). Leaves depth-write to the caller (off).
bool scene_view_begin_oit_pass(SceneView* view) {
    if (!scene_view_oit_ready(view))
        return false;
    SceneViewState* vs = view->state;
    int rw, rh;
    scene_view_render_size(view, &rw, &rh);
    glBindFramebuffer(GL_FRAMEBUFFER, vs->oit_fbo);
    glViewport(0, 0, rw, rh);
    // Only slots 5 (accum) and 6 (revealage) active; the shader's AccumOut /
    // RevealageOut (locations 5/6) land there, FragColor + G-buffer are discarded.
    GLenum bufs[7] = {
        GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_COLOR_ATTACHMENT5, GL_COLOR_ATTACHMENT6};
    glDrawBuffers(7, bufs);
    const GLfloat zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    glClearBufferfv(GL_COLOR, 5, zero); // accum: additive identity
    glClearBufferfv(GL_COLOR, 6, one);  // revealage: full reveal before the product
    glEnablei(GL_BLEND, 5);
    glEnablei(GL_BLEND, 6);
    glBlendFunci(5, GL_ONE, GL_ONE);                  // accum += premultiplied * weight
    glBlendFunci(6, GL_ZERO, GL_ONE_MINUS_SRC_COLOR); // revealage *= (1 - alpha)
    return true;
}

// End the OIT accumulate sub-pass: back to the scene FBO, color-only draw buffer,
// and the standard alpha blend for the refraction sub-pass and later draws.
void scene_view_end_oit_pass(SceneView* view) {
    int rw, rh;
    scene_view_render_size(view, &rw, &rh);
    glBindFramebuffer(GL_FRAMEBUFFER, view->state->framebuffer);
    glViewport(0, 0, rw, rh);
    scene_view_set_draw_buffers(view, false);
    // Undo the indexed blend we set on draw buffers 5/6. The scene MRT now
    // spans slot 7, but 5/6 stay structurally reserved for these OIT targets
    // (the attachment table skips them), so the disables cannot collide.
    glDisablei(GL_BLEND, 5);
    glDisablei(GL_BLEND, 6);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

// Lazy allocation for the moment-based OIT generation targets (spec 11.17):
// b1..b4 (attachment 5) and b0 (attachment 6) as multisample color textures on a
// dedicated FBO sharing the view's depth renderbuffer, plus the single-sample atlas
// the two resolve into. Same lifetime and teardown as the OIT targets beside
// them.
//
// Both generation targets are RGBA32F, including the one that carries a single
// scalar: a multisample resolve blit demands identical formats, and both halves
// blit into the one atlas texture the accumulate pass can afford to sample (see
// the atlas note in scene_view_state.h). The atlas is twice as tall for the same reason.
static bool _ensure_moment_targets(SceneViewState* vs, int rw, int rh) {
    if (vs->moment_fbo != 0 && vs->moment_w == rw && vs->moment_h == rh)
        return true;
    gl_delete_fbo(&vs->moment_fbo);
    gl_delete_texture(&vs->moment_multisample_texture);
    gl_delete_texture(&vs->moment_b0_multisample_texture);
    gl_delete_fbo(&vs->moment_atlas_fbo);
    gl_delete_texture(&vs->moment_atlas_texture);

    const int samples = vs->msaa_samples_built;
    glGenFramebuffers(1, &vs->moment_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, vs->moment_fbo);
    _add_scene_color_attachment(&vs->moment_multisample_texture, GL_RGBA32F, GL_COLOR_ATTACHMENT5,
                                rw, rh, samples);
    // b0 carries one scalar, and above one sample it is padded to RGBA32F
    // anyway: a MULTISAMPLE resolve blit demands identical formats on both
    // sides, and it shares the atlas with the four-channel b1..b4. A
    // single-sample blit only needs compatibility, so at one sample the padding
    // goes -- a quarter of the b0 target, on the path that ships.
    _add_scene_color_attachment(&vs->moment_b0_multisample_texture,
                                samples > 1 ? GL_RGBA32F : GL_R32F, GL_COLOR_ATTACHMENT6, rw, rh,
                                samples);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                              vs->depth_renderbuffer);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    if (ok) {
        glGenTextures(1, &vs->moment_atlas_texture);
        glBindTexture(GL_TEXTURE_2D, vs->moment_atlas_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, rw, rh * 2, 0, GL_RGBA, GL_FLOAT, NULL);
        // Point sampling only: the accumulate pass reads its own pixel, and a
        // filtered tap would mix moments across a silhouette.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &vs->moment_atlas_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, vs->moment_atlas_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               vs->moment_atlas_texture, 0);
        ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

    if (!ok) {
        log_error("OIT moment framebuffer incomplete");
        gl_delete_texture(&vs->moment_multisample_texture);
        gl_delete_texture(&vs->moment_b0_multisample_texture);
        gl_delete_fbo(&vs->moment_fbo);
        gl_delete_texture(&vs->moment_atlas_texture);
        gl_delete_fbo(&vs->moment_atlas_fbo);
        return false;
    }
    vs->moment_w = rw;
    vs->moment_h = rh;
    // Worth a line, because it is the largest single allocation the renderer
    // makes and it buys accuracy rather than resolution: two fp32 targets at the
    // scene's sample count, plus the resolved atlas. fp16 would halve it and
    // costs a third of the accuracy (spec 11.17). The count the targets were
    // actually built at, not the one requested.
    int actual = vs->msaa_samples_actual;
    // Per scene pixel: b1..b4 at 16 bytes a sample, b0 at 16 above one sample
    // and 4 at one (R32F -- see the allocation), and the two-high RGBA32F atlas.
    double b0_bytes = actual > 1 ? 16.0 * actual : 4.0;
    double mb = ((double)rw * rh * (16.0 * actual + b0_bytes + 32.0)) / (1024.0 * 1024.0);
    log_info("OIT moments: %dx%d x%d fp32 + %dx%d atlas (%.0f MB)", rw, rh, actual, rw, rh * 2, mb);
    return true;
}

// Begin the moment generation sub-pass: bind the moment FBO, clear both targets
// to zero, and make both additive. Absorbance adds where transmittance
// multiplies, which is the whole reason the summary can be built in one unsorted
// pass -- so unlike the OIT accumulate beside it, BOTH slots sum here.
bool scene_view_begin_moment_pass(SceneView* view) {
    SceneViewState* vs = view->state;
    int rw, rh;
    scene_view_render_size(view, &rw, &rh);
    if (!_ensure_moment_targets(vs, rw, rh))
        return false;
    glBindFramebuffer(GL_FRAMEBUFFER, vs->moment_fbo);
    glViewport(0, 0, rw, rh);
    GLenum bufs[7] = {
        GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_COLOR_ATTACHMENT5, GL_COLOR_ATTACHMENT6};
    glDrawBuffers(7, bufs);
    const GLfloat zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glClearBufferfv(GL_COLOR, 5, zero);
    glClearBufferfv(GL_COLOR, 6, zero);
    glEnablei(GL_BLEND, 5);
    glEnablei(GL_BLEND, 6);
    glBlendFunci(5, GL_ONE, GL_ONE);
    glBlendFunci(6, GL_ONE, GL_ONE);
    return true;
}

// End the moment generation sub-pass, resolving each multisample target into its
// own half of the atlas the accumulate reads: same size, different destination
// origin, which a multisample blit allows (it forbids scaling, not offsets).
// b1..b4 land in the lower half, b0 in the upper -- MBOIT_ATLAS_B0_TAP is the
// other end of that agreement.
void scene_view_end_moment_pass(SceneView* view) {
    SceneViewState* vs = view->state;
    int rw, rh;
    scene_view_render_size(view, &rw, &rh);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, vs->moment_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, vs->moment_atlas_fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT5);
    glBlitFramebuffer(0, 0, rw, rh, 0, 0, rw, rh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glReadBuffer(GL_COLOR_ATTACHMENT6);
    glBlitFramebuffer(0, 0, rw, rh, 0, rh, rw, rh * 2, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, vs->framebuffer);
    glViewport(0, 0, rw, rh);
    scene_view_set_draw_buffers(view, false);
    glDisablei(GL_BLEND, 5);
    glDisablei(GL_BLEND, 6);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
