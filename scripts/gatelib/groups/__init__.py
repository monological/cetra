"""The gate groups, in the order the suite runs them."""

from gatelib.groups.absorption import run_absorption_gate
from gatelib.groups.alphacov import run_alphacov_gate
from gatelib.groups.anim import run_anim_gate
from gatelib.groups.ao import run_ao_gate
from gatelib.groups.audio import run_audio_gate
from gatelib.groups.camera import run_camera_gate
from gatelib.groups.canvas import run_canvas_gate
from gatelib.groups.catcher import run_catcher_gate
from gatelib.groups.catcher_transparency import run_catcher_transparency_gate
from gatelib.groups.character import run_character_gate
from gatelib.groups.clearcoat import run_clearcoat_gate
from gatelib.groups.cloud_shadow import run_cloud_shadow_gate
from gatelib.groups.clouds import run_cloud_churn_gate
from gatelib.groups.config import run_config_gate
from gatelib.groups.conifer import run_conifer_gate
from gatelib.groups.contact import run_contact_gate
from gatelib.groups.cook import run_cook_gate
from gatelib.groups.crt import run_crt_gate
from gatelib.groups.cull import run_cull_gate
from gatelib.groups.cycle import run_cycle_gate
from gatelib.groups.decals import run_decal_gate
from gatelib.groups.dir_shadow import run_dir_shadow_gate
from gatelib.groups.display import run_display_gate
from gatelib.groups.dither import run_dither_gate
from gatelib.groups.draw_distance import run_draw_distance_gate
from gatelib.groups.draw_list import run_draw_list_gate
from gatelib.groups.emissive import run_emissive_gate
from gatelib.groups.exposure import run_exposure_gate
from gatelib.groups.fit_clip import run_fit_clip_gate
from gatelib.groups.fixture_gen import run_fixture_gen_gate
from gatelib.groups.flare import run_flare_gate
from gatelib.groups.fog_volume import run_fog_volume_gate
from gatelib.groups.fogdepth import run_fogdepth_gate
from gatelib.groups.forest import (
    run_cluster_gate, run_forest_gate, run_prepass_gate, run_quadtree_gate,
)
from gatelib.groups.gait import run_gait_gate
from gatelib.groups.gamepad import run_gamepad_gate
from gatelib.groups.gate_docs import run_gate_docs_gate
from gatelib.groups.glare import run_glare_gate
from gatelib.groups.grain import run_grain_gate
from gatelib.groups.graph import run_graph_gate
from gatelib.groups.grazing import run_grazing_gate
from gatelib.groups.hair import run_hair_flow_gate
from gatelib.groups.idle import run_idle_gate
from gatelib.groups.ies import run_ies_gate
from gatelib.groups.ik import run_ik_gate
from gatelib.groups.imports import _run_import_gates
from gatelib.groups.island import run_island_gate
from gatelib.groups.ladder import run_ladder_gate
from gatelib.groups.layers import run_layers_gate
from gatelib.groups.light_pool import run_light_pool_gate
from gatelib.groups.light_specular import run_light_specular_gate
from gatelib.groups.lighting_stream import run_gi_cook_gate, run_lighting_stream_gate
from gatelib.groups.local_exposure import run_local_exposure_gate
from gatelib.groups.lod import run_lod_gate
from gatelib.groups.lut import run_lut_gate
from gatelib.groups.mask import run_mask_gate
from gatelib.groups.moon import run_moon_gate
from gatelib.groups.night_floor import run_nightfloor_gate
from gatelib.groups.occlusion import run_occlusion_gate
from gatelib.groups.oit import run_oit_gate
from gatelib.groups.origin import run_origin_gate
from gatelib.groups.ortho import run_ortho_gate
from gatelib.groups.overcast import run_overcast_gate
from gatelib.groups.overdraw import run_overdraw_gate
from gatelib.groups.pbr_variant import run_pbr_variant_gate
from gatelib.groups.pcss import run_pcss_gate
from gatelib.groups.probe_set import run_probe_set_gate
from gatelib.groups.profiler import run_profiler_gate
from gatelib.groups.purkinje import run_purkinje_gate
from gatelib.groups.ragdoll import run_ragdoll_gate
from gatelib.groups.rain import run_fog_miss_gate, run_rain_gate
from gatelib.groups.region import run_region_gate
from gatelib.groups.save import run_save_gate
from gatelib.groups.scale import run_scale_gates
from gatelib.groups.scale_twin import run_scale_twin_gate
from gatelib.groups.shader_hooks import run_shader_hooks_gate
from gatelib.groups.shadow_lag import run_shadow_lag_gate
from gatelib.groups.shadows import run_penumbra_gate, run_shadow_tiles_gate
from gatelib.groups.skin import (
    run_skin_area_gate, run_skin_curvature_gate, run_skin_handoff_gate, run_skin_offpath_gate,
    run_sss_banding_gate, run_sss_invariance_gate,
)
from gatelib.groups.sss_tag import run_sss_tag_gate
from gatelib.groups.stars import run_stars_gate
from gatelib.groups.submission import run_submission_gate
from gatelib.groups.taa_ortho import run_taa_ortho_gate
from gatelib.groups.terrain import run_terrain_gate, run_terrain_stream_gate
from gatelib.groups.texcomp import run_texcomp_gate
from gatelib.groups.textures_in import run_textures_in_gate
from gatelib.groups.transform import run_transform_walk_gate
from gatelib.groups.translucent import run_translucent_offpath_gate, run_translucent_shadow_gate
from gatelib.groups.ui import run_ui_gate
from gatelib.groups.varying import run_varying_gate
from gatelib.groups.water import run_beach_gate, run_water_gate, run_water_night_gate
from gatelib.groups.wind_uv import run_wind_uv_gate


GATE_GROUPS = [
    ("scale", "scale invariance (lights x1000, exposure /1000):", run_scale_gates),
    ("scale-twin", "the same scene shrunk 1024 times, to the pixel (spec 13.57):",
     run_scale_twin_gate),
    ("fit-clip", "a fitted camera's near and far planes (spec 13.57):", run_fit_clip_gate),
    ("pcss", "a short occluder keeps its soft shadow (spec 13.58):", run_pcss_gate),
    ("penumbra", "area shadow (analytic penumbra):", run_penumbra_gate),
    ("grazing", "punctual grazing (leak wall base):", run_grazing_gate),
    ("dir-shadow", "cascade shadow (analytic ellipse):", run_dir_shadow_gate),
    ("catcher", "catcher over a real ground (contact fixture):", run_catcher_gate),
    ("contact", "contact shadows for the lights with no shadow map (spec 11.56):",
     run_contact_gate),
    ("light-specular", "a light's specular share (a light that lights diffusely; spec 13.30):",
     run_light_specular_gate),
    ("shadow-tiles", "cached point-light shadows (tiles, views, movers; spec 13.16):",
     run_shadow_tiles_gate),
    ("ao", "ambient occlusion reaches the frame (spec 11.75):", run_ao_gate),
    ("textures-in", "a headless run starts with its textures in (spec 14.8):",
     run_textures_in_gate),
    ("texcomp", "block-compressed textures (format, budget, lever; spec 11.85):",
     run_texcomp_gate),
    ("clearcoat", "clearcoat (the coat normal, its map and its storage; spec 11.86):",
     run_clearcoat_gate),
    ("pbr-variant", "lit-surface variants (a material compiles what it can use; spec 11.93):",
     run_pbr_variant_gate),
    ("ies", "IES photometric profiles (table, symmetry, seeding, fold; spec 11.57):",
     run_ies_gate),
    ("catcher-transparency", "catcher vs transparency (panel through the plane):",
     run_catcher_transparency_gate),
    ("oit", "order-independent transparency (analytic card stack):", run_oit_gate),
    ("absorption", "volume absorption (path length and channel selectivity, spec 11.32):",
     run_absorption_gate),
    ("fog-volume", "local fog volumes (density, tint, arming; spec 11.39):",
     run_fog_volume_gate),
    ("fogdepth", "fog at the translucent depth (spec 11.78):", run_fogdepth_gate),
    ("cloud-shadow", "cloud shadows into the fog and onto the ground (specs 11.39, 11.41):",
     run_cloud_shadow_gate),
    ("stars", "night star field (contribution, ramp, extinction, deck; spec 11.79):",
     run_stars_gate),
    ("night-floor", "the night-sky floor lighting the world (spec 11.80):",
     run_nightfloor_gate),
    ("overcast", "the overcast deck: CIE dome, sun taken away, fog and IBL (spec 13.7):",
     run_overcast_gate),
    ("cycle", "the day/night clock and its sliced env re-bake (spec 11.81):",
     run_cycle_gate),
    ("moon", "the moon: disc, derived phase, terminator and a second casting light "
             "(spec 11.82):", run_moon_gate),
    ("water", "water surface (determinism, absorption, shoreline, reach; specs 11.32-11.35):",
     run_water_gate),
    ("beach", "the shoaling bed the eye can see (surf ring, turquoise, bound; spec 11.44):",
     run_beach_gate),
    ("water-night", "the sea after dark: which light it takes, and what it does with none "
     "(spec 11.84):",
     run_water_night_gate),
    # "rainfall" rather than "rain": --only matches by substring, and "rain" is inside
    # "terrain", which would take the terrain groups and the forest app along every time.
    ("rainfall", "rain: the physics it derives and the state it accumulates (spec 13.9):",
     run_rain_gate),
    ("emissive", "emissive geometry as area lights (fit, intent, light; spec 11.49):",
     run_emissive_gate),
    ("probe-set", "clustered specular probes (selection, blend, tenancy; spec 11.70):",
     run_probe_set_gate),
    # "lighting-stream" rather than "stream": --only matches by substring, and "stream" is inside
    # "terrain-stream", which would take the terrain groups along every time.
    ("lighting-stream", "lighting data that streams (residency, kept texels, tiles; spec 13.24):",
     run_lighting_stream_gate),
    ("gi-cook", "converged lighting kept on disk (GI sweeps, probe columns; spec 13.42):",
     run_gi_cook_gate),
    ("fog-miss", "fog cells without history while the camera turns (spec 13.45):",
     run_fog_miss_gate),
    ("decals", "clustered decals (projection, fade, surface, masks; spec 11.73):",
     run_decal_gate),
    ("config", "the session dumped to JSON and restored from it (spec 11.71):",
     run_config_gate),
    ("clouds", "cloud layer (steady-state churn, report-only):", run_cloud_churn_gate),
    ("skin-offpath", "pre-integrated skin (off-path byte identity):", run_skin_offpath_gate),
    ("skin-curvature", "pre-integrated skin (curvature ordering):", run_skin_curvature_gate),
    ("skin-handoff", "pre-integrated skin (handoff past the scatter ceiling):",
     run_skin_handoff_gate),
    ("skin-area", "subsurface under an area light (spec 11.19 / B3.2):", run_skin_area_gate),
    ("hair", "hair lobes driven by the strand map (spec 11.20 / B8):", run_hair_flow_gate),
    ("flare", "lens flare and chromatic aberration (spec 11.21 / B7):", run_flare_gate),
    ("glare", "the aperture's diffraction glare (spec 13.4):", run_glare_gate),
    ("local-exposure", "local exposure, an exposure per pixel (spec 13.19):",
     run_local_exposure_gate),
    ("sss-invariance", "subsurface blur (world width vs frame size):", run_sss_invariance_gate),
    ("sss-banding", "subsurface blur (kernel not visible as rings):", run_sss_banding_gate),
    ("dither", "output dither (8-bit contour bands, spec 11.24 / E1):", run_dither_gate),
    ("grain", "film grain (new every frame, and its strength; spec 13.28):", run_grain_gate),
    ("crt", "the CRT (lines, mask and tone, tube, dither last; spec 13.28):", run_crt_gate),
    ("lut", "3D LUT colour grading (spec 11.58 / E2):", run_lut_gate),
    ("shader-hooks", "an app's own shaders at named points (spec 13.29):", run_shader_hooks_gate),
    ("purkinje", "Purkinje / scotopic shift (spec 11.83 / B14):", run_purkinje_gate),
    ("origin", "a world away from the origin, and one that moves under it (spec 11.62 / D11):",
     run_origin_gate),
    ("terrain", "Heightfield terrain and erosion (spec 11.59 / D6-D8):", run_terrain_gate),
    ("cook", "the derived-data cook (transparent cache + --cook pre-warm; spec 11.99):",
     run_cook_gate),
    ("terrain-stream", "terrain streaming (spec 11.69 / D4):", run_terrain_stream_gate),
    ("layers", "layered surfaces and their composite cache (specs 11.60, 11.66):",
     run_layers_gate),
    ("translucent", "translucent shadows (analytic layer stack, spec 11.26 / C1):",
     run_translucent_shadow_gate),
    ("translucent-offpath", "translucent shadows (off-path identity and the inverse arm):",
     run_translucent_offpath_gate),
    ("profiler", "gpu timing (per-pass queries, spec 11.27 / E4):", run_profiler_gate),
    ("cull", "wind and skinned geometry is cullable (spec 11.53):", run_cull_gate),
    ("occlusion", "occlusion culling (authored occluders, masked software depth, spec 11.98):",
     run_occlusion_gate),
    ("draw-distance", "a node's draw distance, the camera's alone (spec 13.38):",
     run_draw_distance_gate),
    ("submission", "submission (draw counts + the CPU column, spec 11.28 / E5):",
     run_submission_gate),
    ("draw-list", "draw list (submission order, spec 11.28 Phase 3):", run_draw_list_gate),
    ("transform", "the transform walk (once a frame, spec 11.96):", run_transform_walk_gate),
    ("shadow-lag", "a shadow tracks this frame's pose (spec 11.96):", run_shadow_lag_gate),
    ("lod", "LOD chains (selection by projected size, spec 11.28 Phase 6):", run_lod_gate),
    ("mask", "alpha mask (binary above the cutoff, spec 11.31):", run_mask_gate),
    ("alphacov", "mip alpha coverage (a cutout keeps its area with distance, spec 11.87):",
     run_alphacov_gate),
    ("ladder", "the alpha mip ladder (the dither on the live path, spec 11.101):",
     run_ladder_gate),
    ("taa-ortho", "the TAA jitter under an orthographic camera (spec 11.103):",
     run_taa_ortho_gate),
    ("ortho", "orthographic post and shading (spec 11.104):", run_ortho_gate),
    ("light-pool", "the clustered light index pool (64 KB, its overflow warning; spec 13.51):",
     run_light_pool_gate),
    ("wind-uv", "UV1 carries wind data through import (spec 11.51):", run_wind_uv_gate),
    ("varying", "varyings under partial coverage (spec 11.38):", run_varying_gate),
    ("sss-tag", "subsurface profile tag through the MSAA resolve (spec 11.37):",
     run_sss_tag_gate),
    ("overdraw", "depth complexity (a scene whose answer is known, spec 11.31):",
     run_overdraw_gate),
    ("prepass", "depth prepass (identical picture, less shading, spec 11.30 / E6):",
     run_prepass_gate),
    ("cluster", "cluster-DAG level of detail (spec 11.63):", run_cluster_gate),
    ("quadtree", "the terrain quadtree (spec 11.63):", run_quadtree_gate),
    ("region", "region residency (spec 11.63):", run_region_gate),
    ("island", "the island (spec 11.63):", run_island_gate),
    ("forest", "forest (scattered content: batching, ordering, LOD, spec 11.29):",
     run_forest_gate),
    ("conifer", "the conifer form of the tree generator (spec 13.35):", run_conifer_gate),
    ("gamepad", "gamepad input (a scripted pad through the action layer, spec 11.109):",
     run_gamepad_gate),
    ("audio", "audio (offline PCM through the spatializer, spec 12.0):", run_audio_gate),
    ("anim", "animation blending (a blend space, a crossfade, a masked layer, spec 12.1):",
     run_anim_gate),
    ("graph", "the animation state machine (states, transitions, binding; spec 12.20):",
     run_graph_gate),
    ("ui", "the game UI layer (layout, navigation, capture, settings; spec 12.2):",
     run_ui_gate),
    ("display", "display modes (placement, monitors, what a settings file may do; spec 12.15):",
     run_display_gate),
    ("ragdoll", "a rig falling over (build, shapes, scale, simulation; spec 12.16):",
     run_ragdoll_gate),
    ("camera", "the camera through a scripted pointer (drag, pan, release; spec 12.19):",
     run_camera_gate),
    ("canvas", "the 2D canvas (areas, ramp, thin lines, four twins, fills, text; spec 14.0):",
     run_canvas_gate),
    ("idle", "idle mode (a frame only when something asks: still, timed, thread, input; 14.1):",
     run_idle_gate),
    ("gait", "a walker's gait (stride, footfalls, the eye's lag and bob, a climb; spec 13.47):",
     run_gait_gate),
    ("character", "a character's capsule changed while it stands (crouch, stand, feet; spec 13.48):",
     run_character_gate),
    ("save", "save serialization (entities, spawners, drops, migrations; spec 12.3):",
     run_save_gate),
    ("ik", "two-bone IK (reach, clamp, the singular bind pose, the pole; spec 12.4):",
     run_ik_gate),
    ("import", "import:", _run_import_gates),
    ("fixture-gen", "fixture generators (every gen_*.py reproduces its asset):",
     run_fixture_gen_gate),
    ("exposure", "histogram auto-exposure (the meter, its mask, its percentiles):",
     run_exposure_gate),
    ("gate-docs", "gate docstrings (the documented arm list is the one that runs):",
     run_gate_docs_gate),
]
