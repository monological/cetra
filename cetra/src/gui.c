#include <math.h>
#include <stdio.h>
#include <string.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "gui.h"

#include "config_snapshot.h"
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

/*
 * One spectral wave train's sliders (spec 11.48). Shared by the wind sea and the swell,
 * which carry the same fields -- and a TreeNode pushes its own ID scope, so the two calls
 * can use identical labels without ImGui keying them to one widget.
 *
 * Every one of these re-seeds the initial spectrum on the next frame, which is why they
 * are folded away rather than sitting open.
 */
static void _water_train_gui(const char* label, WaterWaveTrain* train) {
    if (!igTreeNode_Str(label))
        return;
    // No amplitude: a train takes its height from the wind and the fetch, so calm is a
    // lower wind speed rather than a smaller number. `Scale` is a spectral density weight
    // for balancing the two trains, not that missing amplitude -- at 0 the train is gone.
    igSliderFloat("Wind speed (m/s)", &train->wind_speed, 0.5f, 30.0f, "%.1f", 0);
    igSliderFloat("Fetch (m)", &train->fetch, 1000.0f, 500000.0f, "%.0f",
                  ImGuiSliderFlags_Logarithmic);
    igSliderFloat("Direction (rad)", &train->direction, -3.14159265f, 3.14159265f, "%.2f", 0);
    igSliderFloat("Scale", &train->scale, 0.0f, 2.0f, "%.2f", 0);
    igSliderFloat("Peak enhancement", &train->peak_enhancement, 1.0f, 7.0f, "%.2f", 0);
    igSliderFloat("Focus", &train->focus, 0.0f, 1.0f, "%.2f", 0);
    igSliderFloat("Spread gain", &train->spread_gain, 0.1f, 2.0f, "%.2f", 0);
    igSliderFloat("Spread blend", &train->spread_blend, 0.0f, 1.0f, "%.2f", 0);
    igTreePop();
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

            // The cycle first, because it GOVERNS the two sliders below it --
            // a checkbox that greys controls belongs above them, not 35 lines
            // further down. Nothing here calls a bake: the tick owns that and
            // reads these next frame. Day Length 0 freezes the clock, which is
            // what makes Time of Day a scrub rather than a starting point, and
            // the two clock controls are dead with the cycle off, so they say
            // so the same way the sun sliders do.
            igCheckbox("Day Cycle", &sky->cycle_enabled);
            igBeginDisabled(!sky->cycle_enabled);
            igSliderFloat("Day Length", &sky->cycle_day_seconds, 0.0f, 600.0f, "%.0f s", 0);
            float tod_hour = (float)sky->cycle_hour;
            if (igSliderFloat("Time of Day", &tod_hour, 0.0f, 24.0f, "%.2f h", 0))
                sky->cycle_hour = (double)tod_hour;
            igEndDisabled();

            // The cycle OWNS the sun while it runs (spec 11.81): its tick
            // rewrites both angles every frame, so a slider fighting it is a
            // trap rather than a control. Left bound to the live fields
            // rather than replaced by text, so they stay a readout of where
            // the clock has put the sun.
            igBeginDisabled(sky->cycle_enabled);
            sun_moved |=
                igSliderFloat("Sun Elevation", &sky->sun_elevation_deg, -18.0f, 89.0f, "%.1f deg",
                              0);
            sun_released |= igIsItemDeactivatedAfterEdit();
            sun_moved |=
                igSliderFloat("Sun Azimuth", &sky->sun_azimuth_deg, 0.0f, 360.0f, "%.1f deg", 0);
            sun_released |= igIsItemDeactivatedAfterEdit();
            igEndDisabled();
            if (sky->cycle_enabled) {
                igSameLine(0, -1);
                igTextDisabled("(the day cycle owns the sun)");
            }
            // Disc size feeds only the analytic background discs (sampled
            // live); neither is in the env cube, so it needs no re-bake. It
            // sizes BOTH sky bodies since 11.82, which is why it is no longer
            // called "Sun Disc".
            igSliderFloat("Disc Size", &sky->sun_disc_deg, 0.1f, 3.0f, "%.2f deg", 0);
            // Stars are sampled live like the disc; no re-bake on any of it.
            igCheckbox("Stars", &sky->stars_enabled);
            igSliderFloat("Star Brightness", &sky->stars_brightness, 0.0f, 4.0f, "%.2f", 0);
            // The moon is live too -- the disc is analytic and the light is a
            // per-frame rewrite, so nothing here touches a bake. Its angles
            // are greyed under a running cycle for the sun sliders' reason
            // above: the tick owns them and a fighting slider is a trap.
            igCheckbox("Moon", &sky->moon_enabled);
            igSliderFloat("Moon Brightness", &sky->moon_brightness, 0.0f, 4.0f, "%.2f", 0);
            // Draws the moon oversize, which is licence rather than physics --
            // the disc and its halo only, never the light.
            igSliderFloat("Moon Size", &sky->moon_size, 0.5f, 12.0f, "%.1fx", 0);
            igBeginDisabled(sky->cycle_enabled);
            igSliderFloat("Moon Elevation", &sky->moon_elevation_deg, -18.0f, 89.0f, "%.1f deg",
                          0);
            igSliderFloat("Moon Azimuth", &sky->moon_azimuth_deg, 0.0f, 360.0f, "%.1f deg", 0);
            igEndDisabled();
            // The phase as a READOUT, in the one place a user would look for a
            // slider to set it. It is the angle between the two bodies, so
            // there is nothing here to author -- moving either one moves it.
            if (sky->moon_enabled)
                igTextDisabled("phase %.0f%% lit", 100.0f * sky_moon_lit_fraction(sky));
            // The floor lives in the baked LUT (spec 11.80), so its edits
            // ride the sun's own re-bake chain -- the established price of
            // touching baked sky state.
            bool floor_toggled = igCheckbox("Night Floor", &sky->night_floor_enabled);
            sun_moved |= floor_toggled;
            sun_released |= floor_toggled;
            sun_moved |= igSliderFloat("Floor Brightness", &sky->night_floor_brightness,
                                       0.0f, 4.0f, "%.2f", 0);
            sun_released |= igIsItemDeactivatedAfterEdit();
            if (sun_moved)
                sky_update_sun(sky, scene->ibl, engine);
            if (sun_released)
                scene_environment_changed(scene, engine);

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
                    scene_environment_changed(scene, engine);
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
        ReflectionProbeSet* probe_set = scene->probe_set;
        if (probe_set && probe_set->count == 1) {
            ReflectionProbe* probe = probe_set->probes[0];
            _begin_effect_group("Reflection Probe", &probe->enabled);
            igSliderFloat("Probe Intensity", &probe->intensity, 0.0f, 4.0f, "%.2f", 0);
            igSliderFloat("Box Fade", &probe->box_fade, 0.0f, 0.5f, "%.2f", 0);
            igCheckbox("Show Capture", &probe->debug_background);
            _end_effect_group();
        } else if (probe_set && probe_set->count > 1) {
            // The descriptors upload with the froxel masks every frame, so an
            // edit here is live on the next one with no invalidation path.
            if (igCollapsingHeader_BoolPtr("Reflection Probes", NULL, 0)) {
                for (int i = 0; i < probe_set->count; ++i) {
                    ReflectionProbe* probe = probe_set->probes[i];
                    igPushID_Int(i);
                    igText("Probe %d  (%.1f, %.1f, %.1f)", i, probe->position[0],
                           probe->position[1], probe->position[2]);
                    igSliderFloat("Intensity", &probe->intensity, 0.0f, 4.0f, "%.2f", 0);
                    igSliderFloat("Box Fade", &probe->box_fade, 0.0f, 0.5f, "%.2f", 0);
                    igPopID();
                }
            }
        }

        // Decals (spec 11.73). Everything here is live on the next frame with no
        // invalidation path, for the reason the probe block above says: the
        // descriptors are packed and uploaded with the froxel masks every build.
        // The image is not editable, because a decal's image is a layer of the
        // material texture array and swapping one is a rebuild of that array.
        if (scene->decal_count > 0 && igCollapsingHeader_BoolPtr("Decals", NULL, 0)) {
            for (int i = 0; i < scene->decal_count; ++i) {
                Decal* decal = &scene->decals[i];
                igPushID_Int(i);
                igText("Decal %d", i);
                igCheckbox("Enabled", &decal->enabled);
                igDragFloat3("Position", decal->position, 0.05f, -1000.0f, 1000.0f, "%.2f", 0);
                igDragFloat3("Half Extent", decal->half_extent, 0.02f, 0.01f, 100.0f, "%.2f", 0);
                igDragFloat3("Direction", decal->direction, 0.01f, -1.0f, 1.0f, "%.2f", 0);
                igSliderFloat("Opacity", &decal->opacity, 0.0f, 1.0f, "%.2f", 0);
                igSliderFloat("Angle Fade", &decal->angle_fade, 0.0f, 89.0f, "%.0f deg", 0);
                igSliderFloat("Feather", &decal->feather, 0.0f, 1.0f, "%.2f", 0);
                igSliderFloat("Normal Strength", &decal->normal_strength, 0.0f, 2.0f, "%.2f", 0);
                igPopID();
            }
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
            // Two controls because the two are different quantities: a reflectance the
            // light drives, and a radiance added regardless.
            //
            // The albedo gets SLIDERS rather than a colour wheel. Water's is a few per
            // cent -- the library default is (0.0038, 0.0219, 0.0321) -- which a picker
            // renders as a black swatch you cannot aim, and its useful range is far below
            // the 0..1 a picker spans. The glow stays a picker, and HDR, because it is
            // scene radiance and can legitimately exceed 1.
            igSliderFloat3("Scatter Albedo", water->scatter_albedo, 0.0f, 0.2f, "%.4f", 0);
            igColorEdit3("Scatter Glow", water->scatter_glow,
                         ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
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
                // Depth is the water's, not a train's, so it sits outside both.
                igSliderFloat("Sea depth (m)", &water->sea.sea_depth, 1.0f, 500.0f, "%.0f",
                              ImGuiSliderFlags_Logarithmic);
                _water_train_gui("Wind sea", &water->sea.wind_sea);
                _water_train_gui("Swell", &water->sea.swell);
            }
            igCheckbox("Caustics", &water->caustics);
            igCheckbox("Sun glitter", &water->glitter);
            if (fft) {
                igCheckbox("Foam history", &water->foam_history);
                igSliderFloat("Foam decay", &water->foam_decay, 0.05f, 2.0f, "%.2f", 0);
            }
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

        // What the meter actually decided, which nothing showed until 11.52 --
        // the exposure was three numbers multiplied together and the GUI
        // displayed one of the inputs. Shown whether or not adaptation is on:
        // with it off this reads the camera alone, which is the answer to "why
        // is the frame this bright" either way.
        {
            Exposure* ex = &engine->exposure;
            float gain = exposure_auto_gain(ex);
            // `--` rather than 0 when nothing has been metered. Printing the
            // uninitialised luminance shows "Metered 0 cd/m2", which is a real
            // reading and a wrong one -- the same argument lum_reduce_frag makes
            // about not emitting 0 for an empty histogram, and the state it is
            // describing is exactly that one.
            if (ex->adapted_valid)
                igText("Metered %.4g cd/m2   gain %.4f   EV100 %.2f",
                       (double)ex->adapted_luminance, (double)gain,
                       (double)exposure_ev100(ex));
            else
                igText("Metered --   gain %.4f   EV100 %.2f", (double)gain,
                       (double)exposure_ev100(ex));
            if (igIsItemHovered(0))
                igSetTooltip("Metered scene luminance, the adaptation gain it produced, and the "
                             "camera's EV100. Gain is capped at 1: auto-exposure only darkens.");
        }

        if (igTreeNode_Str("Metering")) {
            static const char* const meter_names[] = {"Uniform", "Centre-weighted", "Spot"};
            int mode = (int)engine->exposure.meter_mode;
            if (igCombo_Str_arr("Mode", &mode, meter_names,
                                (int)(sizeof(meter_names) / sizeof(meter_names[0])), -1))
                engine->exposure.meter_mode = (MeteringMode)mode;
            if (engine->exposure.meter_mode != METERING_UNIFORM) {
                igSliderFloat("Radius", &engine->exposure.meter_radius, 0.05f, 1.0f, "%.2f",
                              ImGuiSliderFlags_AlwaysClamp);
                if (igIsItemHovered(0))
                    igSetTooltip("Fraction of the frame's half-diagonal, in UV -- an ellipse in "
                                 "pixels on a non-square frame.");
            }
            // Low is deliberately allowed up to 0.95 and high down to 0.05; the
            // shader falls back to the whole population if they cross, so a
            // slider drag through the middle degrades to a plain mean instead of
            // to a black frame.
            igSliderFloat("Low %", &engine->exposure.meter_low, 0.0f, 0.95f, "%.2f",
                          ImGuiSliderFlags_AlwaysClamp);
            if (igIsItemHovered(0))
                igSetTooltip("Fraction of the DARKEST pixels ignored. High by default: a meter "
                             "that includes the black background is measuring the background.");
            igSliderFloat("High %", &engine->exposure.meter_high, 0.05f, 1.0f, "%.2f",
                          ImGuiSliderFlags_AlwaysClamp);
            if (igIsItemHovered(0))
                igSetTooltip("Everything above this fraction is ignored -- the highlight tail.");
            // Per FRAME, not per second, and said once above both rather than in a
            // tooltip -- igIsItemHovered refers to the LAST item, so a single
            // tooltip after two sliders describes only the second.
            igTextDisabled("Adaptation rate, per frame");
            igSliderFloat("Adapt Up", &engine->exposure.adapt_rate_up, 0.001f, 1.0f, "%.3f",
                          ImGuiSliderFlags_AlwaysClamp);
            igSliderFloat("Adapt Down", &engine->exposure.adapt_rate_down, 0.001f, 1.0f, "%.3f",
                          ImGuiSliderFlags_AlwaysClamp);
            igTreePop();
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
        static const char* const spec_occ_names[] = {"Off", "Legacy", "Split"};
        // The combo writes its INDEX straight back into the enum, so a label
        // dropped here without the enum value (or the reverse) compiles clean
        // and silently selects the wrong mode. config_snapshot.c guards its own
        // vocabulary this way; this one had nothing until spec 11.77 removed a
        // label from both and noticed only one side would have complained.
        _Static_assert(sizeof(spec_occ_names) / sizeof(*spec_occ_names) ==
                           POSTFX_SPEC_OCC_SPLIT + 1,
                       "spec_occ_names must label every PostFXSpecOccMode");
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
        // The pass needs something to march toward -- a shadow-casting
        // directional, or a local light that has no shadow map to be sharpened
        // against. Say so rather than let the toggle look inert.
        if (!postfx_contact_shadows_have_light(fx))
            igTextDisabled("(needs a directional or a map-less local light)");
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
        igCheckbox("Occlusion Culling", &engine->occlusion_cull_enabled);
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

        // Grouped here and applied earlier still, for the reason above: the
        // retina's spectral response sits after the lens and before the tone
        // curve. Visible only on a dim frame -- the global gate is an exact zero
        // in daylight, so dragging these in a lit scene does nothing, by design.
        _begin_effect_group("Purkinje Shift", &fx->purkinje_enabled);
        igSliderFloat("Rod Blend", &fx->purkinje_strength, 0.0f, 1.0f, "%.2f", 0);
        // Narrower than the CLI's [-30, 30] and [0, 4] on purpose: these are the
        // usable range, and a slider spanning the validation bound would put the
        // whole shipping look in a few pixels of travel. A restored snapshot
        // beyond a slider's end is clamped the first time it is dragged.
        igSliderFloat("Mesopic Bias", &fx->purkinje_bias_ev, -12.0f, 12.0f, "%.1f stops", 0);
        igSliderFloat("Rod Acuity Loss", &fx->purkinje_acuity, 0.0f, 2.0f, "%.2f", 0);
        igSliderFloat("Rod Noise", &fx->purkinje_noise, 0.0f, 2.0f, "%.2f", 0);
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

        // The LUT has no enable checkbox because the loaded texture IS the
        // enable, and no file picker because loading one is the only control
        // here that would need a file read rather than a uniform write --
        // --lut and a scene file's post.lut are the two ways in. What is
        // exposed is what a uniform can carry.
        if (fx->lut_texture) {
            igSeparatorText("Color LUT");
            igTextDisabled("%s (%d\xc2\xb3)", fx->lut_name, fx->lut_size);
            igSliderFloat("LUT Strength", &fx->lut_strength, 0.0f, 1.0f, "%.2f", 0);
            int interp = (int)fx->lut_interp;
            static const char* const interp_names[] = {"Trilinear", "Tetrahedral"};
            if (igCombo_Str_arr("LUT Interp", &interp, interp_names,
                                (int)(sizeof(interp_names) / sizeof(interp_names[0])), -1))
                fx->lut_interp = (PostFXLutInterp)interp;
        }

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
        // The whole tuned state, where Print Camera above is the pose alone
        // (spec 11.71). Fixed filename in the working directory rather than a
        // path field: there is no text input in this panel, and the point of the
        // button is that it takes one click while the frame is on screen.
        igSameLine(0, -1);
        if (igButton("Dump Config", (ImVec2){0, 0}))
            config_snapshot_save(engine, scene, "cetra_config.json");
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
            igTableNextColumn();
            igText("PERIOD");
            igTableNextColumn();
            igText("%.3f ms", profiler_period_ms(engine->profiler));
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
