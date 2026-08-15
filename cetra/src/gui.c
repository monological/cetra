#include <math.h>
#include <stdio.h>
#include <string.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "gui.h"

#include "engine.h"
#include "profiler.h"
#include "ext/log.h"
#include "gi_volume.h"
#include "light.h"
#include "light_cluster.h"
#include "material.h"
#include "postfx.h"
#include "probe.h"
#include "water.h"
#include "render.h"
#include "springbone.h"
#include "scene.h"
#include "shadow.h"
#include "sky.h"
#include "texture.h"
#include "wind.h"

// Dear ImGui via cimgui. CIMGUI_DEFINE_ENUMS_AND_STRUCTS gives the full
// struct/enum definitions to C; CIMGUI_USE_GLFW/OPENGL3 (compile defs from
// CMake) expose the GLFW + OpenGL3 backend bindings in cimgui_impl.h.
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"
#include "cimgui_impl.h"


// Dark panel with an indigo (#5f5fff) accent: the accent drives every
// interactive element (title bar, buttons, collapsing headers, checkmarks,
// sliders) while the surface stays a near-black blue-tinted charcoal.
void gui_apply_style(void) {
    ImGuiStyle* style = igGetStyle();
    igStyleColorsDark(style);

    style->WindowRounding = 6.0f;
    style->ChildRounding = 4.0f;
    style->FrameRounding = 4.0f;
    style->PopupRounding = 4.0f;
    style->GrabRounding = 4.0f;
    style->ScrollbarRounding = 4.0f;
    style->WindowBorderSize = 1.0f;
    style->FrameBorderSize = 1.0f;
    style->WindowPadding = (ImVec2){14.0f, 14.0f};
    style->FramePadding = (ImVec2){9.0f, 6.0f};
    style->ItemSpacing = (ImVec2){9.0f, 10.0f};
    style->ItemInnerSpacing = (ImVec2){8.0f, 6.0f};

    const ImVec4 accent = {0.373f, 0.373f, 1.000f, 1.00f}; // #5f5fff
    const ImVec4 accent_hi = {0.500f, 0.500f, 1.000f, 1.00f};

    ImVec4* c = style->Colors;
    c[ImGuiCol_Text] = (ImVec4){0.90f, 0.90f, 0.94f, 1.00f};
    c[ImGuiCol_TextDisabled] = (ImVec4){0.50f, 0.50f, 0.55f, 1.00f};
    c[ImGuiCol_WindowBg] = (ImVec4){0.055f, 0.055f, 0.075f, 0.97f};
    c[ImGuiCol_ChildBg] = (ImVec4){0.00f, 0.00f, 0.00f, 0.00f};
    c[ImGuiCol_PopupBg] = (ImVec4){0.070f, 0.070f, 0.095f, 0.98f};
    c[ImGuiCol_Border] = (ImVec4){0.267f, 0.267f, 0.267f, 0.50f}; // #444, inactive

    c[ImGuiCol_FrameBg] = (ImVec4){0.130f, 0.130f, 0.170f, 1.00f};
    c[ImGuiCol_FrameBgHovered] = (ImVec4){0.200f, 0.200f, 0.300f, 1.00f};
    c[ImGuiCol_FrameBgActive] = (ImVec4){0.260f, 0.260f, 0.420f, 1.00f};

    c[ImGuiCol_TitleBg] = (ImVec4){0.080f, 0.080f, 0.110f, 1.00f};
    c[ImGuiCol_TitleBgActive] = (ImVec4){0.190f, 0.190f, 0.400f, 1.00f};
    c[ImGuiCol_TitleBgCollapsed] = (ImVec4){0.080f, 0.080f, 0.110f, 0.75f};

    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_hi;

    c[ImGuiCol_Button] = (ImVec4){0.373f, 0.373f, 1.000f, 0.55f};
    c[ImGuiCol_ButtonHovered] = (ImVec4){0.373f, 0.373f, 1.000f, 0.90f};
    c[ImGuiCol_ButtonActive] = accent_hi;

    c[ImGuiCol_Header] = (ImVec4){0.373f, 0.373f, 1.000f, 0.45f};
    c[ImGuiCol_HeaderHovered] = (ImVec4){0.373f, 0.373f, 1.000f, 0.70f};
    c[ImGuiCol_HeaderActive] = (ImVec4){0.373f, 0.373f, 1.000f, 0.90f};

    c[ImGuiCol_Separator] = (ImVec4){0.267f, 0.267f, 0.267f, 0.60f};
    c[ImGuiCol_SeparatorHovered] = (ImVec4){0.373f, 0.373f, 1.000f, 0.78f};
    c[ImGuiCol_SeparatorActive] = accent_hi;

    c[ImGuiCol_ResizeGrip] = (ImVec4){0.373f, 0.373f, 1.000f, 0.25f};
    c[ImGuiCol_ResizeGripHovered] = (ImVec4){0.373f, 0.373f, 1.000f, 0.55f};
    c[ImGuiCol_ResizeGripActive] = (ImVec4){0.373f, 0.373f, 1.000f, 0.90f};

    c[ImGuiCol_TextSelectedBg] = (ImVec4){0.373f, 0.373f, 1.000f, 0.40f};
}

// A checkbox that enables a group of dependent parameters. The parameters stay
// visible but greyed out and inert until the effect is on — so toggling never
// reflows the panel — and are indented one level so the grouping is obvious.
// The enabling checkbox itself stays interactive. Pair with _end_effect_group().
static void _begin_effect_group(const char* label, bool* enabled) {
    igCheckbox(label, enabled);
    igBeginDisabled(!*enabled);
    igIndent(0.0f);
}

static void _end_effect_group(void) {
    igUnindent(0.0f);
    igEndDisabled();
}

// Combo entry for one light: "1: KeyLamp (Spot)", or the type alone when the
// asset gave the light no name. The index leads because it is the only part
// guaranteed unique -- ImGui keys widgets by label, so two identically named
// lights would otherwise share one selectable.
static void _light_gui_label(char* out, size_t out_size, const Light* light, int index) {
    if (!light)
        snprintf(out, out_size, "%d: <empty>", index);
    else if (light->name && light->name[0])
        snprintf(out, out_size, "%d: %s (%s)", index, light->name, light_type_name(light->type));
    else
        snprintf(out, out_size, "%d: %s", index, light_type_name(light->type));
}

// Combo entry for one material. Same reason the index leads as for lights:
// ImGui keys widgets by label, and glTF happily ships two materials called
// "Material.001".
static void _material_gui_label(char* out, size_t out_size, const Material* material, int index) {
    if (!material)
        snprintf(out, out_size, "%d: <empty>", index);
    else if (material->name && material->name[0])
        snprintf(out, out_size, "%d: %s", index, material->name);
    else
        snprintf(out, out_size, "%d: <unnamed>", index);
}

// One control for one row of the shared material vocabulary. Every row gets one
// rather than a hand-written widget per property, so a property added for the
// scene file turns up here for free and the two cannot disagree about what
// exists.
static void _material_param_control(Material* material, const MaterialParam* p) {
    // A texture is read-only here: the panel has no file picker, and showing
    // whether one is bound is more use than a control that cannot bind it.
    if (p->type == MATERIAL_PARAM_TEXTURE) {
        igTextDisabled("%s: %s", p->key,
                       material_param_texture(material, p) ? "bound" : "none (author in a .cscn)");
        return;
    }

    float v[3] = {0};
    material_param_get(material, p, v);
    bool changed = false;
    switch (p->type) {
    case MATERIAL_PARAM_COLOR:
        changed = igColorEdit3(p->key, v, ImGuiColorEditFlags_Float);
        break;
    case MATERIAL_PARAM_FLOAT:
        changed = igSliderFloat(p->key, v, p->min, p->max, "%.3f", 0);
        break;
    case MATERIAL_PARAM_INT: {
        // Named where the table names them: dragging an integer to reach "leaf"
        // asks the reader to know a numbering nothing shows them.
        int iv = (int)v[0];
        changed = p->enum_labels
                      ? igCombo_Str_arr(p->key, &iv, p->enum_labels, p->enum_count, -1)
                      : igSliderInt(p->key, &iv, (int)p->min, (int)p->max, "%d", 0);
        v[0] = (float)iv;
        break;
    }
    case MATERIAL_PARAM_TEXTURE:
        break; // handled above
    }
    if (changed)
        material_param_set(material, p, v);
}

// The material property editor, in its own window so it can be moved, resized
// and closed. `open` is the caller's persistent flag and doubles as the close
// button's target.
static void _engine_gui_material_window(Material* material, int index, bool* open) {
    // "###" pins the window ID while the visible title tracks the selection.
    // Without it a rename or a different material would read as a new window
    // and lose the position and size the user put it in.
    char label[128], title[160];
    _material_gui_label(label, sizeof(label), material, index);
    snprintf(title, sizeof(title), "%s###material_editor", label);
    if (!igBegin(title, open, 0)) {
        igEnd();
        return;
    }

    // Scope every control to the selected material. ImGui keys widgets by
    // label within a window, and these labels are the material vocabulary
    // rather than strings chosen for this panel -- so without a scope a future
    // property could silently collide with something else. Keying on the
    // selection also stops a drag on one material leaving state on the next.
    igPushID_Int(index);

    // Every row of the shared vocabulary gets a control, rather than a
    // hand-written widget per property: a table that both the scene file and
    // this window read cannot drift, and a property added for one of them turns
    // up in the other for free.
    //
    // Walked group by group in table order, so a property only has to name its
    // group to land in the right place. The first group is the one every
    // surface has and opens by default; the rest describe features a material
    // opts into, and stay shut.
    // Consumed a group at a time so the indent push and pop are lexically
    // paired: a loop that carried the open state across iterations would leak an
    // indent level into every panel below the moment someone added an early exit
    // to the row body.
    for (size_t i = 0; i < MATERIAL_PARAM_COUNT;) {
        const char* group = MATERIAL_PARAMS[i].group;
        bool group_open = igCollapsingHeader_TreeNodeFlags(
            group, i == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        if (group_open)
            igIndent(0.0f);
        for (; i < MATERIAL_PARAM_COUNT && strcmp(MATERIAL_PARAMS[i].group, group) == 0; i++) {
            if (group_open)
                _material_param_control(material, &MATERIAL_PARAMS[i]);
        }
        if (group_open)
            igUnindent(0.0f);
    }

    igPopID();
    igEnd();
}

// The one "environment changed on release" chain, shared by the sun sliders
// and the cloud controls so the downstream consumers cannot drift apart:
// re-bake the env WITH clouds when the layer is on (the per-drag path never
// pays that march), refresh a probe that only mirrors the sky, and re-arm
// the GI volume. A scene-captured probe (--probe-scene) is left stale --
// re-rendering the scene per release is too costly; its baked reflections
// stay as shot. The GI sweep re-arms on RELEASE, not per drag frame,
// because a sweep is one scene render per probe face -- the one cost the
// converge-then-idle cadence exists to keep off the steady state; unlike
// the probe it does not stall (spread over following frames at `rate`, old
// atlas sampleable throughout).
static void _sky_release_rebake(Engine* engine, Scene* scene, SkyAtmosphere* sky) {
    // Clouds existed this session -> the env may need the deck added or
    // purged (enabled implies noise_baked on every reachable path).
    if (sky->clouds.noise_baked)
        sky_bake_ex(sky, scene->ibl, engine, sky->clouds.enabled);
    if (scene->probe) {
        if (scene->probe->cubemap == 0)
            reflection_probe_capture(scene->probe, engine, scene, 0.1f, 100.0f, true);
        else
            log_info("Sky: scene-captured probe not refreshed on release");
    }
    gi_volume_mark_dirty(scene->gi_volume);
}

// The main settings panel, in Dear ImGui. Sections are collapsing headers;
// effect on/off states are checkboxes and their parameters appear indented
// beneath them. Bound directly to the same engine/scene/postfx fields.
static void _engine_gui_panel(Engine* engine) {
    Camera* camera = engine->camera;
    igSetNextWindowPos((ImVec2){15, 15}, ImGuiCond_FirstUseEver, (ImVec2){0, 0});
    igSetNextWindowSize((ImVec2){320, 660}, ImGuiCond_FirstUseEver);
    if (!igBegin("Cetra", NULL, 0)) {
        igEnd();
        return;
    }

    // --- View: render mode leads (the primary control), then the display
    // overlays on one row (checkboxes so their on/off state is visible) and the
    // camera mode. Global viewport controls, so they lead the panel.
    static const char* const render_modes[] = {
        "PBR",        "Normals", "World Pos",       "Tex Coords",         "Tangent Space",
        "Flat Color", "Albedo",  "Simple Lighting", "Metallic/Roughness", "Velocity",
        "HDR Hotspots", "SSS Hotspots", "Extrapolation"};
    int rm = engine->current_render_mode;
    int render_mode_count = (int)(sizeof(render_modes) / sizeof(render_modes[0]));
    if (igCombo_Str_arr("Render Mode", &rm, render_modes, render_mode_count, -1))
        engine->current_render_mode = (RenderMode)rm;

    bool show_xyz = engine->show_xyz;
    if (igCheckbox("XYZ", &show_xyz))
        set_engine_show_xyz(engine, show_xyz);
    igSameLine(0, -1);
    bool wireframe = engine->show_wireframe;
    if (igCheckbox("Wireframe", &wireframe))
        set_engine_show_wireframe(engine, wireframe);
    igSameLine(0, -1);
    igCheckbox("Bones", &engine->show_bones);
    igSameLine(0, -1);
    igCheckbox("Lights", &engine->show_lights);

    if (igRadioButton_Bool("Free", engine->camera_mode == CAMERA_MODE_FREE))
        engine->camera_mode = CAMERA_MODE_FREE;
    igSameLine(0, -1);
    if (igRadioButton_Bool("Orbit", engine->camera_mode == CAMERA_MODE_ORBIT))
        engine->camera_mode = CAMERA_MODE_ORBIT;

    // --- Animation: a collapsing section like the effect stacks below, shown
    // only when a clip is loaded.
    AnimationState* anim = get_render_animation_state();
    if (anim && igCollapsingHeader_TreeNodeFlags("Animation", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (igButton(anim->playing ? "Pause Animation" : "Play Animation", (ImVec2){0, 0})) {
            if (anim->playing)
                pause_animation(anim);
            else
                play_animation(anim);
        }
        if (anim->skeleton && igButton("Recalc Bind Pose", (ImVec2){0, 0}))
            recalculate_inverse_bind_poses(anim->skeleton);

        if (anim->springs &&
            igCollapsingHeader_TreeNodeFlags("Spring Bones", ImGuiTreeNodeFlags_DefaultOpen)) {
            SpringBoneSystem* sb = anim->springs;
            if (igCheckbox("Springs Enabled", &sb->enabled) && sb->enabled)
                spring_bone_reset(sb); // re-enable snaps instead of lurching
            igSliderFloat("Stiffness", &sb->params.stiffness, 0.0f, 1.0f, "%.3f", 0);
            igSliderFloat("Damping", &sb->params.damping, 0.0f, 1.0f, "%.3f", 0);
            igSliderFloat("Gravity", &sb->params.gravity, 0.0f, 30.0f, "%.2f", 0);
        }
    }

    static int mat_sel = 0;
    static bool mat_editor_open = false;
    Scene* scene = get_current_scene(engine);
    // Clamped here rather than at each reader: a scene swap can leave the index
    // past the new count, and the section below only runs when its header is
    // expanded while the editor window reads it either way.
    if (!scene || mat_sel >= (int)scene->material_count)
        mat_sel = 0;

    if (scene && scene->light_count > 0 &&
        igCollapsingHeader_TreeNodeFlags("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
        // The controls below address ONE light. A single slider cannot honestly
        // show two different intensities, and writing every light on a drag
        // flattens whatever key/fill ratio the scene was authored with.
        static int sel = 0;
        if (sel < 0 || sel >= (int)scene->light_count)
            sel = 0; // a scene swap can leave the index past the new count
        if (scene->light_count > 1) {
            char label[128];
            _light_gui_label(label, sizeof(label), scene->lights[sel], sel);
            if (igBeginCombo("Light", label, 0)) {
                for (size_t i = 0; i < scene->light_count; i++) {
                    char item[128];
                    _light_gui_label(item, sizeof(item), scene->lights[i], (int)i);
                    if (igSelectable_Bool(item, (int)i == sel, 0, (ImVec2){0, 0}))
                        sel = (int)i;
                    if ((int)i == sel)
                        igSetItemDefaultFocus();
                }
                igEndCombo();
            }
        }
        Light* light = scene->lights[sel];
        if (light) {
            // Shown in the unit the light was AUTHORED in, so a lamp written as
            // 30 lm in a .cscn reads 30 lm here rather than the 2.4 cd it shades
            // as. Log scale because the useful span is four decades: a domestic
            // lamp and midday sun are both on this one slider.
            static const float UNIT_MAX[] = {
                [LIGHT_UNITS_DEFAULT] = 400.0f, // unreachable; light_display_units resolves it
                [LIGHT_UNITS_CANDELA] = 400.0f, // ~5000 lm isotropic
                [LIGHT_UNITS_LUMENS] = 5000.0f, // a bright domestic fixture
                [LIGHT_UNITS_LUX] = 120000.0f,  // noon sun ~100k lx
                [LIGHT_UNITS_NITS] = 2000.0f,
            };
            LightUnits units = light_display_units(light);
            char fmt[16];
            snprintf(fmt, sizeof(fmt), "%%.1f %s", light_units_name(units));
            float intensity = light_intensity_in_units(light);
            if (igSliderFloat("Intensity", &intensity, 0.0f, UNIT_MAX[units], fmt,
                              ImGuiSliderFlags_Logarithmic))
                set_light_intensity_units(light, intensity, units);
            if (igIsItemHovered(0))
                igSetTooltip("Photometric, in the unit the light was authored in. Point/spot "
                             "shade in candela (lumens / 4pi), sun in lux, panel in nits. "
                             "Falloff is inverse-square, bounded by Range.");

            if (light->type == LIGHT_POINT || light->type == LIGHT_SPOT) {
                float range = light->range;
                if (igSliderFloat("Range", &range, 0.0f, 100.0f, "%.1f m", 0))
                    light->range = range;
                if (igIsItemHovered(0))
                    igSetTooltip("Where the inverse-square falloff is windowed to zero. Also "
                                 "the cull radius. 0 = unbounded.");
            }
        }
        // Clustered-forward occupancy: tint each fragment by how many lights
        // its cluster carries (blue 1 .. red >= 16). Directional lights are
        // shaded unclustered (they reach every fragment), so a scene lit only
        // by a key rig + IBL has nothing to show -- surface the split and grey
        // the toggle out there, rather than letting it look broken.
        if (engine->light_cluster) {
            // Grey-out keys off the scene's area-light count, not the packed
            // count: unchecking zeroes the packed count, which would lock the
            // checkbox off.
            int n_area = 0;
            for (size_t i = 0; i < scene->light_count; i++)
                if (scene->lights[i] && scene->lights[i]->type == LIGHT_AREA)
                    n_area++;
            if (n_area == 0)
                igBeginDisabled(true);
            igCheckbox("Area Lights", &engine->light_cluster->area_lights_enabled);
            if (n_area == 0) {
                igEndDisabled();
                igSameLine(0, -1);
                igTextDisabled("(no area panels in scene)");
            }

            int n_dir = engine->light_cluster->lights.light_counts[0];
            int n_clustered = engine->light_cluster->lights.light_counts[1];
            igText("%d directional (unclustered), %d clustered", n_dir, n_clustered);
            if (n_clustered == 0)
                igBeginDisabled(true);
            igCheckbox("Cluster Heatmap", &engine->cluster_debug);
            if (n_clustered == 0) {
                igEndDisabled();
                igSameLine(0, -1);
                igTextDisabled("(needs point/spot lights)");
            }
        }
    }

    // Material selection lives in this panel; the properties live in their own
    // window (raised after this one ends). Thirty controls inside an already
    // long panel pushes everything after them off the bottom, and a material is
    // the one thing here you tune while watching the frame rather than glance
    // at -- so it gets a window that can be moved, resized and closed.
    if (scene && scene->material_count > 0 &&
        igCollapsingHeader_TreeNodeFlags("Materials", 0)) {
        igIndent(0.0f);

        // Like the light section above, this addresses ONE material: a scene
        // carries dozens and a single slider cannot honestly show two values.
        char label[128];
        _material_gui_label(label, sizeof(label), scene->materials[mat_sel], mat_sel);
        if (igBeginCombo("Material", label, 0)) {
            for (size_t i = 0; i < scene->material_count; i++) {
                char item[128];
                _material_gui_label(item, sizeof(item), scene->materials[i], (int)i);
                if (igSelectable_Bool(item, (int)i == mat_sel, 0, (ImVec2){0, 0}))
                    mat_sel = (int)i;
                if ((int)i == mat_sel)
                    igSetItemDefaultFocus();
            }
            igEndCombo();
        }
        if (igButton("Edit Material", (ImVec2){0, 0}))
            mat_editor_open = true;
        igUnindent(0.0f);
    }

    // Top level, and gated on the shadow system alone. These controls used to
    // live inside Environment, whose header also requires a skybox and an IBL --
    // so on a scene authored with neither, which is what a shadow fixture is
    // (no ambient means the umbra is genuinely black), every shadow control was
    // invisible on exactly the scenes built to judge shadows.
    if (scene && scene->shadow_system &&
        igCollapsingHeader_TreeNodeFlags("Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
        ShadowSystem* ss = scene->shadow_system;
        _begin_effect_group("Enabled", &ss->enabled);
        // Count change takes effect next frame via the depth pass's
        // rebuild-on-change; 1 = the classic scene-fit single map.
        // AlwaysClamp: Ctrl+Click text entry must not escape the range
        // (the count sizes heap array writes in the depth pass).
        igSliderInt("Cascades", &ss->cascade_count, 1, SHADOW_CASCADES, "%d",
                    ImGuiSliderFlags_AlwaysClamp);
        igCheckbox("Cascade Tint", &ss->csm_debug);

        // Toggling this reallocates the depth array (the transmittance layers
        // live in it), so it is a checkbox rather than anything continuous.
        igCheckbox("Translucent Shadows", &ss->tsm_enabled);

        _begin_effect_group("Moment Shadows", &ss->msm_enabled);
        // A combo, not a slider: each distinct value reallocates two array
        // textures, and a continuous drag would rebuild them every frame it
        // moved. Per LAYER, since the layer count is casters times cascades and
        // varies by scene; the allocation logs the total.
        static const char* const msm_sizes[] = {"1024 (16 MB/layer)", "2048 (67 MB/layer)",
                                                "4096 (268 MB/layer)"};
        int msm_size_idx = ss->msm_size >= 4096 ? 2 : (ss->msm_size >= 2048 ? 1 : 0);
        if (igCombo_Str_arr("Moment Size", &msm_size_idx, msm_sizes, 3, -1))
            ss->msm_size = 1024 << msm_size_idx;
        // Blur trades thin-caster occlusion for softness and the exchange rate
        // is steep: on the gate's 0.3-wide pillar band, 0.25 already lets 9% of
        // the light through and 1.0 lets 42% (spec 11.22).
        igSliderFloat("Moment Blur", &ss->msm_blur, 0.0f, 1.0f, "%.2f px", 0);
        igSliderFloat("Moment Bleed", &ss->msm_bleed, 0.0f, 0.5f, "%.2f", 0);
        _end_effect_group();

        // Inert while moments are live: the cascade lookup branches on
        // msmEnabled before it ever reads pcssEnabled, so leaving this
        // interactive would show a checkbox that does nothing.
        igBeginDisabled(ss->msm_enabled);
        _begin_effect_group("PCSS", &ss->pcss_enabled);
        igSliderFloat("Shadow Softness", &ss->pcss_softness, 0.0f, 4.0f, "%.2f", 0);
        _end_effect_group();
        igEndDisabled();
        _end_effect_group();

        _begin_effect_group("Shadow Catcher", &scene->shadow_catcher);
        igSliderFloat("Shadow Strength", &scene->shadow_catcher_strength, 0.0f, 1.0f, "%.2f", 0);
        _end_effect_group();
    }

    if (scene && scene->render_skybox && scene->ibl &&
        igCollapsingHeader_TreeNodeFlags("Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (scene->sky) {
            SkyAtmosphere* sky = scene->sky;
            igSeparatorText("Sky");
            // Elevation dips a few degrees below the horizon for dusk; the
            // atmosphere fades the key light out there. A sun move re-derives
            // the direction and re-bakes the LUT/env/IBL live (cheap at this
            // env size) and retints the coupled key light through one path.
            bool sun_moved = false;
            bool sun_released = false;
            sun_moved |=
                igSliderFloat("Sun Elevation", &sky->sun_elevation_deg, -6.0f, 89.0f, "%.1f deg",
                              0);
            sun_released |= igIsItemDeactivatedAfterEdit();
            sun_moved |=
                igSliderFloat("Sun Azimuth", &sky->sun_azimuth_deg, 0.0f, 360.0f, "%.1f deg", 0);
            sun_released |= igIsItemDeactivatedAfterEdit();
            // Disc size feeds only the analytic background sun (sampled live);
            // it is not in the env cube, so it needs no re-bake.
            igSliderFloat("Sun Disc", &sky->sun_disc_deg, 0.1f, 3.0f, "%.2f deg", 0);
            if (sun_moved)
                sky_update_sun(sky, scene->ibl, engine);
            if (sun_released)
                _sky_release_rebake(engine, scene, sky);

            // Cloud layer (only offered once the noise fields exist). The
            // screen march follows every slider live; the env/IBL copy and
            // its downstream consumers update on RELEASE like the sun (the
            // with-clouds bake is the cost the drag path never pays). Wind
            // re-bakes nothing -- the env cube holds a still of the deck by
            // design.
            if (sky->clouds.noise_baked) {
                bool clouds_were_on = sky->clouds.enabled;
                bool cloud_edit = false;
                _begin_effect_group("Clouds", &sky->clouds.enabled);
                igSliderFloat("Coverage", &sky->clouds.coverage, 0.0f, 1.0f, "%.2f", 0);
                cloud_edit |= igIsItemDeactivatedAfterEdit();
                igSliderFloat("Cloud Type", &sky->clouds.cloud_type, 0.0f, 1.0f, "%.2f", 0);
                cloud_edit |= igIsItemDeactivatedAfterEdit();
                igSliderFloat("Cloud Density", &sky->clouds.density, 0.1f, 3.0f, "%.2f", 0);
                cloud_edit |= igIsItemDeactivatedAfterEdit();
                igSliderFloat("Wind km/h", &sky->clouds.wind_speed_kmh, 0.0f, 300.0f, "%.0f", 0);
                igSliderFloat("Wind Dir", &sky->clouds.wind_dir_deg, 0.0f, 360.0f, "%.0f deg", 0);
                _end_effect_group();
                bool toggled = clouds_were_on != sky->clouds.enabled;
                if (toggled || (cloud_edit && sky->clouds.enabled))
                    _sky_release_rebake(engine, scene, sky);
            }
        }

        _begin_effect_group("Ground Projection", &scene->skybox_ground_projection);
        // Log scale + wide range: the dome radius is a world-space distance
        // apps typically scale to the scene, anywhere from a few units
        // (meter-scale models) to thousands (large-unit assets).
        igSliderFloat("Dome Radius", &scene->skybox_gp_radius, 1.0f, 10000.0f, "%.2f m",
                      ImGuiSliderFlags_Logarithmic);
        igSliderFloat("Capture Height", &scene->skybox_gp_height, 0.1f, 10.0f, "%.2f m", 0);
        _end_effect_group();

        igSliderFloat("IBL Intensity", &scene->ibl->intensity, 0.0f, 4.0f, "%.2f", 0);

        // Attached probes always carry a capture: the toggle switches
        // consumption, it does not recapture
        if (scene->probe) {
            _begin_effect_group("Reflection Probe", &scene->probe->enabled);
            igSliderFloat("Probe Intensity", &scene->probe->intensity, 0.0f, 4.0f, "%.2f", 0);
            igSliderFloat("Box Fade", &scene->probe->box_fade, 0.0f, 0.5f, "%.2f", 0);
            igCheckbox("Show Capture", &scene->probe->debug_background);
            _end_effect_group();
        }

        // Attached water always carries its own bed and cascades; the toggle
        // switches whether the surface draws.
        if (scene->water) {
            Water* water = scene->water;
            _begin_effect_group("Water", &water->enabled);
            // No bed invalidation here: the bake stores height and gradient, neither of
            // which depends on the level -- the level enters in the shader, as
            // waterLevel - bed. Re-arming it from this slider re-baked an identical
            // texture on every dragged frame, which on a real terrain provider is 65,536
            // noise evaluations and a 1 MB upload per frame. The bake now notices its own
            // inputs changing (see _water_bake_bed).
            igSliderFloat("Level", &water->level, -50.0f, 50.0f, "%.2f", 0);
            igSliderFloat("Wavelength", &water->wavelength, 1.0f, 400.0f, "%.1f",
                          ImGuiSliderFlags_Logarithmic);
            igSliderFloat("Amplitude", &water->amplitude, 0.0f, 8.0f, "%.3f",
                          ImGuiSliderFlags_Logarithmic);
            // 1 is the steepest crest whose horizontal map is still injective;
            // the range stops there because past it the surface folds.
            igSliderFloat("Steepness", &water->steepness, 0.0f, 1.0f, "%.2f", 0);
            igSliderFloat("Roughness", &water->roughness, 0.0f, 0.3f, "%.3f", 0);
            igColorEdit3("Absorption", water->absorption, ImGuiColorEditFlags_Float);
            igColorEdit3("Scatter", water->scatter, ImGuiColorEditFlags_Float);
            // Spectral cascades allocate 24 textures and 45 passes a frame, so the
            // switch is offered rather than assumed. Caustics and foam ride on it:
            // both are selected from Jacobian compression, which the Gerstner path
            // does not compute.
            bool fft = water->wave_model == WATER_WAVES_FFT;
            if (igCheckbox("Spectral cascades (FFT)", &fft))
                water->wave_model = fft ? WATER_WAVES_FFT : WATER_WAVES_GERSTNER;
            if (fft) {
                // The sea state, which only the spectral path reads. Moving any of these
                // re-seeds the initial spectrum on the next frame -- a CPU pass over
                // three 128^2 grids, so dragging a slider here costs more than the rest
                // of this panel put together and is why they are folded behind the model
                // that uses them.
                //
                // No amplitude: the spectrum takes its height from the wind and the
                // fetch, so calm is a lower wind speed rather than a smaller number.
                igSliderFloat("Wind speed (m/s)", &water->sea.wind_speed, 0.5f, 30.0f, "%.1f",
                              0);
                igSliderFloat("Fetch (km)", &water->sea.fetch, 1000.0f, 500000.0f, "%.0f",
                              ImGuiSliderFlags_Logarithmic);
                igSliderFloat("Sea depth (m)", &water->sea.sea_depth, 1.0f, 500.0f, "%.0f",
                              ImGuiSliderFlags_Logarithmic);
                igSliderFloat("Peak enhancement", &water->sea.peak_enhancement, 1.0f, 7.0f,
                              "%.2f", 0);
                igSliderFloat("Swell", &water->sea.swell, 0.0f, 1.0f, "%.2f", 0);
            }
            igCheckbox("Caustics", &water->caustics);
            igCheckbox("Sun glitter", &water->glitter);
            _end_effect_group();
        }

        if (scene->gi_volume) {
            GIVolume* gi = scene->gi_volume;
            _begin_effect_group("GI Probe Volume", &gi->enabled);
            igText("%dx%dx%d probes, %dx%d atlas", gi->counts[0], gi->counts[1], gi->counts[2],
                   gi->atlas_w, gi->atlas_h);
            // The one number worth watching: a converged volume reads 0 and does
            // no work at all. Anything else means captures are running.
            igText(gi->dirty_count > 0 ? "converging: %d probes left" : "converged (%d)",
                   gi->dirty_count);
            igSliderInt("Probes / Frame", &gi->rate, 0, 32, "%d", 0);
            igCheckbox("Show Atlas", &gi->debug_atlas);
            if (igButton("Recapture", (ImVec2){0, 0}))
                gi_volume_mark_dirty(gi);
            _end_effect_group();
        }
    }

    if (engine->postfx &&
        igCollapsingHeader_TreeNodeFlags("Post", ImGuiTreeNodeFlags_DefaultOpen)) {
        PostFX* fx = engine->postfx;

        igSeparatorText("Anti-aliasing");
        bool msaa = engine->msaa_samples > 1;
        if (igCheckbox("MSAA 4x", &msaa))
            set_engine_msaa_samples(engine, msaa ? 4 : 1);
        igSameLine(0, -1);
        bool taa = fx->taa_enabled;
        if (igCheckbox("TAA", &taa))
            set_engine_taa_enabled(engine, taa);
        // TAAU render scale. Applied on release, not per drag tick: each change
        // rebuilds every render target and resets the temporal histories (the
        // sun/cloud sliders defer their re-bake the same way). AlwaysClamp
        // because Ctrl+click types a value straight into an allocation size.
        static float pending_scale = 1.0f;
        igSliderFloat("Render Scale", &pending_scale, 0.5f, 1.0f, "%.2f",
                      ImGuiSliderFlags_AlwaysClamp);
        if (igIsItemDeactivatedAfterEdit())
            set_engine_render_scale(engine, pending_scale);
        else if (!igIsItemActive())
            // Not being dragged: track the live value, so a scale the setter
            // clamped or refused shows here rather than the stale request.
            pending_scale = engine->render_scale;
        if (!fx->taa_enabled && engine->render_scale < 1.0f) {
            // Without the resolve there is nothing to reconstruct with, so the
            // frame is magnified rather than upscaled: softer, not faster.
            igTextDisabled("(needs TAA to reconstruct; magnifying)");
        }

        // Index maps to the enum minus PASSTHROUGH, which is not a look and
        // stays out of the picker
        static const char* const tonemap_names[] = {"ACES", "PBR Neutral", "AgX"};
        int tm = (int)fx->tonemap_mode - 1;
        if (igCombo_Str_arr("Tonemap", &tm, tonemap_names,
                            (int)(sizeof(tonemap_names) / sizeof(tonemap_names[0])), -1))
            fx->tonemap_mode = (PostFXTonemapMode)(tm + 1);
        igCheckbox("Auto Exposure", &engine->exposure.automatic);
        // Two controls because they are two quantities, shown one at a time.
        // One slider over one range could not serve both: under a physical
        // camera the value is stops, where the linear range 0.05..8 cannot
        // reach 0 (neutral) and cannot stop down at all.
        if (engine->exposure.physical) {
            igSliderFloat("EV Bias", &engine->exposure.bias_stops, -6.0f, 6.0f, "%+.2f EV", 0);
            if (igIsItemHovered(0))
                igSetTooltip("Exposure compensation on the physical camera. Positive opens up.");
        } else {
            igSliderFloat("Exposure", &engine->exposure.multiplier, 0.05f, 8.0f, "%.2f", 0);
            if (igIsItemHovered(0))
                igSetTooltip("Linear multiplier. Authoring a post.camera in a .cscn switches "
                             "this to an EV bias instead.");
        }

        _begin_effect_group("Bloom", &fx->bloom_enabled);
        igSliderFloat("Bloom Strength", &fx->bloom_strength, 0.0f, 0.1f, "%.3f", 0);
        igSliderFloat("Bloom Threshold", &fx->bloom_threshold, 0.0f, 8.0f, "%.2f", 0);
        _end_effect_group();

        // Under bloom because it reads the same thresholded pyramid, though it
        // no longer needs bloom switched on to get one.
        _begin_effect_group("Lens Flare", &fx->flare_enabled);
        igSliderFloat("Flare Strength", &fx->flare_strength, 0.0f, 0.15f, "%.3f", 0);
        igSliderInt("Ghosts", &fx->flare_ghosts, 0, 8, "%d", 0);
        igSliderFloat("Ghost Spacing", &fx->flare_ghost_spacing, 0.15f, 0.45f, "%.3f", 0);
        igSliderFloat("Halo Width", &fx->flare_halo_width, 0.0f, 1.0f, "%.2f", 0);
        igSliderFloat("Flare Dispersion", &fx->flare_chroma, 0.0f, 12.0f, "%.1f px", 0);
        // Bounded by the pyramid that exists at this resolution rather than a
        // constant: the mip count falls out of the frame size, so a fixed top
        // would let the slider travel past the last real level and do nothing.
        igSliderInt("Source Mip", &fx->flare_source_lod, 0,
                    fx->bloom_mips > 0 ? fx->bloom_mips - 1 : 0, "%d", 0);
        _end_effect_group();

        _begin_effect_group("Ambient Occlusion (GTAO)", &fx->ssao_enabled);
        // Log scale + wide range: the AO/GI reach is a world-space distance, so
        // apps scale it to the scene (meter-scale models sit near the bottom,
        // large-unit scenes in the hundreds).
        igSliderFloat("AO Radius", &fx->ssao_radius, 0.05f, 1000.0f, "%.2f m",
                      ImGuiSliderFlags_Logarithmic);
        igSliderFloat("AO Strength", &fx->ssao_strength, 0.0f, 1.0f, "%.2f", 0);
        static const char* const spec_occ_names[] = {"Off", "Legacy", "Bent normal", "Split"};
        int so = (int)fx->spec_occlusion_mode;
        if (igCombo_Str_arr("Specular Occlusion", &so, spec_occ_names,
                            (int)(sizeof(spec_occ_names) / sizeof(spec_occ_names[0])), -1))
            fx->spec_occlusion_mode = (PostFXSpecOccMode)so;
        igCheckbox("AO Edge Filter", &fx->ao_edge_filter_enabled);
        _end_effect_group();

        _begin_effect_group("Contact Shadows", &fx->contact_shadows_enabled);
        igSliderFloat("CS Strength", &fx->cs_strength, 0.0f, 1.0f, "%.2f", 0);
        // Log scale + wide range: the march reach is a world-space distance the
        // app scales to the scene, like SSR Distance.
        igSliderFloat("CS Distance", &fx->cs_distance, 0.01f, 1000.0f, "%.3f m",
                      ImGuiSliderFlags_Logarithmic);
        // The pass needs a shadow-casting directional (its published direction);
        // say so rather than let the toggle look inert.
        if (fx->fog_light_count == 0)
            igTextDisabled("(needs a shadow-casting directional light)");
        _end_effect_group();

        _begin_effect_group("SSR", &fx->ssr_enabled);
        igSliderFloat("SSR Strength", &fx->ssr_strength, 0.0f, 2.0f, "%.2f", 0);
        // Log scale + wide range: the march reach is a world-space distance
        // the app scales to the scene (a couple of units on meter-scale
        // models, thousands on large-unit assets)
        igSliderFloat("SSR Distance", &fx->ssr_max_distance, 1.0f, 20000.0f, "%.2f m",
                      ImGuiSliderFlags_Logarithmic);
        igSliderFloat("SSR Floor Rough", &fx->ssr_floor_roughness, 0.0f, 1.0f, "%.2f", 0);
        bool ssr_full_res = fx->ssr_full_res;
        if (igCheckbox("SSR Full Res", &ssr_full_res))
            postfx_set_ssr_full_res(fx, ssr_full_res);
        igCheckbox("SSR Temporal (needs TAA)", &fx->ssr_temporal);
        igCheckbox("SSR Denoise", &fx->ssr_denoise);
        igSliderFloat("SSR Jitter", &fx->ssr_jitter, 0.0f, 0.2f, "%.3f", 0);
        _end_effect_group();

        _begin_effect_group("SSGI", &fx->ssgi_enabled);
        igSliderFloat("GI Intensity", &fx->ssgi_intensity, 0.0f, 4.0f, "%.2f", 0);
        _end_effect_group();

        _begin_effect_group("Volumetric Fog", &fx->fog_enabled);
        // Log scale + wide range: density and falloff are world-space
        // quantities apps scale to the scene (tiny values on large-unit
        // assets) — the SSR Distance slider precedent
        igSliderFloat("Fog Density", &fx->fog_density, 0.00001f, 1.0f, "%.5f",
                      ImGuiSliderFlags_Logarithmic);
        igSliderFloat("Height Falloff", &fx->fog_height_falloff, 0.05f, 5000.0f, "%.2f",
                      ImGuiSliderFlags_Logarithmic);
        igSliderFloat("Anisotropy", &fx->fog_anisotropy, -0.9f, 0.9f, "%.2f", 0);
        igSliderFloat("Sun Boost", &fx->fog_sun_boost, 0.0f, 8.0f, "%.2f", 0);
        // The volume's own depth range, which is NOT the camera's: the mapping
        // is exponential, so a near pinned to the clip plane spends most of the
        // slices on air in front of the lens. 0 derives it from Far.
        igSliderFloat("Volume Near", &fx->fog_near, 0.0f, 20.0f, "%.2f",
                      ImGuiSliderFlags_Logarithmic);
        igSliderFloat("Volume Far", &fx->fog_far, 1.0f, 5000.0f, "%.1f",
                      ImGuiSliderFlags_Logarithmic);
        igSliderFloat("Depth Distribution", &fx->fog_depth_dist, 0.25f, 4.0f, "%.2f", 0);
        igSliderFloat("Temporal Blend", &fx->fog_temporal_blend, 0.0f, 0.98f, "%.2f", 0);
        // Grid XY is what resolves a beam's silhouette; a change reallocates the
        // volumes and restarts their history at the next fog frame.
        igSliderInt("Grid X", &fx->froxel_grid_x, 40, 640, "%d", 0);
        igSliderInt("Grid Y", &fx->froxel_grid_y, 24, 360, "%d", 0);
        igSliderInt("Grid Z", &fx->froxel_grid_z, 16, 256, "%d", 0);
        // Shadow softness in the medium. k is the exponential's sharpness: high
        // converges back toward the hard binary compare, low goes soft and
        // starts leaking light through blockers. Sweeping it IS the demo.
        igCheckbox("ESM Shadows", &fx->fog_esm_enabled);
        igSliderFloat("ESM Sharpness", &fx->fog_esm_k, 5.0f, 200.0f, "%.0f",
                      ImGuiSliderFlags_Logarithmic);
        // Editing takes ownership away from the sky, which otherwise republishes
        // its own zenith radiance every frame and the picker would snap back.
        if (igColorEdit3("Fog Ambient", fx->fog_ambient, 0) && scene && scene->sky)
            scene->sky->publish_fog_ambient = false;
        _end_effect_group();

        igCheckbox("Normals G-buffer", &fx->normals_enabled);
        igSliderFloat("Spec AA", &engine->specular_aa_strength, 0.0f, 2.0f, "%.2f", 0);
        igCheckbox("Energy Comp", &engine->energy_comp_enabled);
        igCheckbox("Clearcoat", &engine->clearcoat_enabled);
        igCheckbox("Specular", &engine->specular_enabled);
        igCheckbox("Sheen", &engine->sheen_enabled);
        igCheckbox("Parallax (POM)", &engine->parallax_enabled);
        igCheckbox("Subsurface (SSS)", &engine->sss_enabled);
        /*
         * The scatter RADIUS, per profile, live.
         *
         * It is postfx state rather than material state, so the material editor cannot reach
         * it -- and it is the one number spec 11.14 names as the proximate cause of the
         * blown-out square it recorded and did not fix. Dropping it from 1.5 to 0.25 hid that
         * artifact; the mechanism above the ceiling was left in place. A knob nobody can turn
         * cannot be swept, and a cause that cannot be swept cannot be confirmed.
         *
         * The whole row group hides when there are no profiles, which is every scene that
         * authors no subsurface material.
         */
        if (engine->postfx && engine->postfx->sss_profile_count > 0) {
            igIndent(0.0f);
            for (int p = 0; p < engine->postfx->sss_profile_count; p++) {
                char label[32];
                snprintf(label, sizeof(label), "SSS Radius %d", p);
                igSliderFloat(label, &engine->postfx->sss_profiles[p][3], 0.0f, 2.0f, "%.3f", 0);
            }
            igUnindent(0.0f);
        }
        igCheckbox("Skin Pre-integration", &engine->skin_preint_enabled);
        // Moment weighting is a better weight INSIDE the accumulate, not a
        // second transparency path, so it greys out with the pass it rides.
        _begin_effect_group("OIT", &engine->oit_enabled);
        igCheckbox("OIT Moment Weighting", &engine->oit_moments_enabled);
        _end_effect_group();

        igCheckbox("Instancing", &engine->instancing_enabled);
        igCheckbox("Sort Opaque Front-to-Back", &engine->opaque_sort_enabled);
        igCheckbox("Depth Prepass", &engine->depth_prepass_enabled);
        // The bias greys out with selection because it has no meaning without
        // it: level 0 is level 0 at any distance.
        _begin_effect_group("LOD", &engine->lod_enabled);
        igSliderFloat("LOD Bias", &engine->lod_bias, 0.25f, 4.0f, "%.2f", 0);
        _end_effect_group();
    }

    if (engine->postfx &&
        igCollapsingHeader_TreeNodeFlags("Finishing", ImGuiTreeNodeFlags_DefaultOpen)) {
        PostFX* fx = engine->postfx;

        _begin_effect_group("Vignette", &fx->vignette_enabled);
        igSliderFloat("Vig Strength", &fx->vignette_strength, 0.0f, 1.0f, "%.2f", 0);
        igSliderFloat("Vig Radius", &fx->vignette_radius, 0.0f, 1.0f, "%.2f", 0);
        _end_effect_group();

        // Grouped with the other lens artifacts even though it is applied far
        // earlier in the shader -- a lens effect, so it acts on the light
        // before the sensor rather than in the finishing block these sit in.
        _begin_effect_group("Chromatic Aberration", &fx->ca_enabled);
        igSliderFloat("CA Pixels", &fx->ca_strength, 0.0f, 40.0f, "%.1f px", 0);
        _end_effect_group();

        _begin_effect_group("Sharpen", &fx->sharpen_enabled);
        igSliderFloat("Sharpen Amount", &fx->sharpen_strength, 0.0f, 3.0f, "%.2f", 0);
        _end_effect_group();

        _begin_effect_group("Grain", &fx->grain_enabled);
        igSliderFloat("Grain Amount", &fx->grain_strength, 0.0f, 0.3f, "%.3f", 0);
        _end_effect_group();

        _begin_effect_group("Dither", &fx->dither_enabled);
        igSliderFloat("Dither LSB", &fx->dither_strength, 0.0f, 4.0f, "%.2f", 0);
        _end_effect_group();

        _begin_effect_group("Color Grade", &fx->grade_enabled);
        igDragFloat3("Lift", fx->grade_lift, 0.005f, -1.0f, 1.0f, "%.3f", 0);
        igDragFloat3("Gamma", fx->grade_gamma, 0.01f, 0.1f, 4.0f, "%.3f", 0);
        igDragFloat3("Gain", fx->grade_gain, 0.01f, 0.0f, 4.0f, "%.3f", 0);
        _end_effect_group();

        _begin_effect_group("Depth of Field", &fx->dof_enabled);
        igCheckbox("Autofocus", &fx->dof_autofocus);
        // Manual focus distance is only meaningful when autofocus is off.
        igBeginDisabled(fx->dof_autofocus);
        igSliderFloat("Focus Dist", &fx->dof_focus_distance, 0.0f, 100.0f, "%.2f", 0);
        igEndDisabled();
        igSliderFloat("Focus Range", &fx->dof_focus_range, 0.1f, 100.0f, "%.2f", 0);
        igSliderFloat("Max CoC", &fx->dof_max_coc, 0.0f, 40.0f, "%.1f", 0);
        igSliderInt("Blades", &fx->dof_blades, 0, 9, "%d", 0);
        igSliderFloat("Rotation", &fx->dof_rotation, 0.0f, 180.0f, "%.0f deg", 0);
        _end_effect_group();

        _begin_effect_group("Motion Blur", &fx->motion_blur_enabled);
        igSliderFloat("Shutter", &fx->motion_blur_scale, 0.0f, 2.0f, "%.2f", 0);
        _end_effect_group();
    }

    if (camera && igCollapsingHeader_TreeNodeFlags("Camera Transform", 0)) {
        igDragFloat3("Look At", camera->look_at, 0.1f, -100.0f, 100.0f, "%.2f", 0);
        igDragFloat3("Up", camera->up_vector, 0.1f, -25.0f, 25.0f, "%.2f", 0);
        igSliderFloat("Distance", &camera->distance, 0.0f, 3000.0f, "%.2f", 0);
        igSliderFloat("Height", &camera->height, -2000.0f, 2000.0f, "%.1f", 0);
        igSliderFloat("Theta", &camera->theta, 0.0f, GLM_PI_2, "%.3f", 0);
        igSliderFloat("Phi", &camera->phi, 0.0f, GLM_PI_2, "%.3f", 0);
        igSliderFloat("FOV", &camera->fov_radians, 0.1f, GLM_PI, "%.3f", 0);
        igSliderFloat("Zoom Speed", &camera->zoom_speed, 0.0f, 2.0f, "%.3f", 0);
        igSliderFloat("Orbit Speed", &camera->orbit_speed, 0.0f, 0.1f, "%.4f", 0);
        igSliderFloat("Amplitude", &camera->amplitude, 0.0f, 50.0f, "%.2f", 0);
        igSliderFloat("Near Clip", &camera->near_clip, 0.01f, 100.0f, "%.3f", 0);
        igSliderFloat("Far Clip", &camera->far_clip, 0.1f, 10000.0f, "%.1f", 0);
        // Camera diagnostic: overlay the live pose next to the FPS, and print an
        // exact-repro CLI line to stdout so a grazing interactive view can be
        // reproduced headlessly (--cam-eye/--cam-target override the orbit framing).
        igCheckbox("Camera Info (HUD)", &engine->show_camera_hud);
        if (igButton("Print Camera", (ImVec2){0, 0})) {
            printf("-W %d -H %d --fov %.1f --cam-eye %.3f,%.3f,%.3f "
                   "--cam-target %.3f,%.3f,%.3f --cam-up %.3f,%.3f,%.3f\n",
                   engine->fb_width, engine->fb_height, glm_deg(camera->fov_radians),
                   camera->position[0], camera->position[1], camera->position[2],
                   camera->look_at[0], camera->look_at[1], camera->look_at[2], camera->up_vector[0],
                   camera->up_vector[1], camera->up_vector[2]);
            fflush(stdout);
        }
    }

    // Per-pass GPU time, CPU time and submission counts (specs 11.27, 11.28).
    // Present only when the flag built the profiler; there is nothing to show
    // and nothing to say otherwise.
    if (engine->profiler && igCollapsingHeader_TreeNodeFlags("Profiler", 0)) {
        if (igBeginTable("gpu_timing", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp,
                         (ImVec2){0, 0}, 0.0f)) {
            igTableNextColumn();
            igText("pass");
            igTableNextColumn();
            igText("GPU");
            igTableNextColumn();
            igText("CPU");
            int rows = profiler_row_count(engine->profiler);
            for (int row = 0; row < rows; row++) {
                igTableNextColumn();
                igText("%s", profiler_row_name(engine->profiler, row));
                igTableNextColumn();
                igText("%.3f ms", profiler_row_ms(engine->profiler, row));
                igTableNextColumn();
                igText("%.3f ms", profiler_row_cpu_ms(engine->profiler, row));
            }
            igTableNextColumn();
            igText("TIMED");
            igTableNextColumn();
            igText("%.3f ms", profiler_total_ms(engine->profiler));
            igTableNextColumn();
            igText("%.3f ms", profiler_cpu_total_ms(engine->profiler));
            igTableNextColumn();
            igText("FRAME (wall)");
            igTableNextColumn();
            igText("%.3f ms", profiler_frame_ms(engine->profiler));
            igTableNextColumn();
            igText(" ");
            igEndTable();
        }

        int submit_cols = profiler_submit_row_count();
        if (igBeginTable("submission", submit_cols + 1,
                         ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp,
                         (ImVec2){0, 0}, 0.0f)) {
            igTableNextColumn();
            igText("pass");
            for (int col = 0; col < submit_cols; col++) {
                igTableNextColumn();
                igText("%s", profiler_submit_row_name(col));
            }
            for (int row = 0; row < profiler_row_count(engine->profiler); row++) {
                const SubmitStats* s = profiler_row_submit(engine->profiler, row);
                if (!s || s->meshes_seen == 0)
                    continue;
                igTableNextColumn();
                igText("%s", profiler_row_name(engine->profiler, row));
                for (int col = 0; col < submit_cols; col++) {
                    igTableNextColumn();
                    igText("%zu", submit_stat_value(s, col));
                }
            }
            SubmitStats total = profiler_submit_total(engine->profiler);
            igTableNextColumn();
            igText("TOTAL");
            for (int col = 0; col < submit_cols; col++) {
                igTableNextColumn();
                igText("%zu", submit_stat_value(&total, col));
            }
            igEndTable();
        }
    }

    // Read while this window is still current, to place the material editor
    // beside it rather than on top of it.
    ImVec2 panel_pos = igGetWindowPos();
    ImVec2 panel_size = igGetWindowSize();

    igEnd();

    // A sibling window, so it opens outside the main panel rather than pushing
    // everything after it off the bottom. Raised after igEnd() because a
    // material can disappear under it -- a scene swap leaves the index stale,
    // and the bounds check has to run against whatever scene is current now.
    if (mat_editor_open && scene && scene->material_count > 0) {
        if (scene->materials[mat_sel]) {
            // Both FirstUseEver, so this is a starting point and never fights a
            // window the user has since moved or resized (or one restored from
            // imgui.ini).
            //
            // The size is not cosmetic. A window with no size auto-fits its
            // contents, but a slider's width is derived FROM the window width,
            // so the two settle on something far too narrow and every label
            // clips -- the property names are the widest thing here and they
            // sit to the right of each control. 470 clears the longest of them
            // at ImGui's default 65/35 split.
            igSetNextWindowPos((ImVec2){panel_pos.x + panel_size.x + 12.0f, panel_pos.y},
                               ImGuiCond_FirstUseEver, (ImVec2){0, 0});
            igSetNextWindowSize((ImVec2){470.0f, 560.0f}, ImGuiCond_FirstUseEver);
            _engine_gui_material_window(scene->materials[mat_sel], mat_sel, &mat_editor_open);
        }
    }
}

// Frameless FPS readout pinned to the top-right corner, with an optional live
// camera-pose block (show_camera_hud) beneath it. FPS-only keeps the original
// 90px/transparent placement byte-for-byte.
static void _engine_gui_fps_overlay(Engine* engine) {
    bool cam = engine->show_camera_hud && engine->camera;
    float width = cam ? 300.0f : 90.0f;
    igSetNextWindowPos((ImVec2){(float)engine->win_width - width, 10.0f}, ImGuiCond_Always,
                       (ImVec2){0, 0});
    igSetNextWindowBgAlpha(cam ? 0.35f : 0.0f);
    if (igBegin("##fps", NULL,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_AlwaysAutoResize)) {
        if (engine->show_fps)
            igText("%.1f FPS", engine->fps);
        if (cam) {
            Camera* c = engine->camera;
            igText("eye    %.1f %.1f %.1f", c->position[0], c->position[1], c->position[2]);
            igText("target %.1f %.1f %.1f", c->look_at[0], c->look_at[1], c->look_at[2]);
            igText("fov %.1f  dist %.1f  %dx%d", glm_deg(c->fov_radians),
                   glm_vec3_distance(c->position, c->look_at), engine->fb_width, engine->fb_height);
        }
    }
    igEnd();
}

// Build and render the engine's ImGui panels. NewFrame ran at the top of the
// render loop and latched gui_frame_active; this adds the windows and flushes
// the draw data, gated on the same latch so igRender always pairs with it.
void gui_render_frame(Engine* engine) {
    if (!engine || !engine->gui_frame_active)
        return;

    if (engine->show_gui && engine->camera)
        _engine_gui_panel(engine);
    if (engine->show_fps || engine->show_camera_hud)
        _engine_gui_fps_overlay(engine);

    igRender();
    ImGui_ImplOpenGL3_RenderDrawData(igGetDrawData());
}
