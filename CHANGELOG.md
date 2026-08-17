# Changelog

## v0.12.0 — 2026-08-17

- **Cloud shadows on the ground** — the deck's sun-transmittance map reaches `pbr_frag`, the shadow catcher and water's caustics, riding a sampler unit the opaque pass never uses.
- **Local fog volumes** — world-space boxes of denser air with an inward feather and a tint, authored in `.cscn` and folded σ-weighted into the froxel medium.
- **A sun on the water** — a Cox-Munk lobe, wind-anisotropic and shadowed, with far-field roughness derived from the spectrum's own slope variance.
- **Foam that lingers** — whitewater accumulates per cascade texel so it outlives the crest that made it, mipped and read with `textureGrad`.
- **A simulated swash** — the run-up is a traced shoreline with an alongshore coordinate, several waves interfering rather than one line moving, riding a uniform block.
- **Whitecaps that break** — breaking is selected on the wave's own forward face rather than as a filled height contour, composited as a whitecap instead of a swash.
- **The swell gets its own spectrum** — a second wave train with its own wind and fetch, authorable per scene, replacing a hardcoded 8.4 m/s gale no scene could reach.
- **By-example texturing** — histogram-preserving stochastic sampling stops the sand tiling visibly, at no sampler cost.
- **Absorption at world scale** — extinction is per world unit and the path clamp is a budget in extinction lengths, so a world where a unit is not a metre is no longer six times too absorbing.

## v0.11.0 — 2026-08-14

- **A water surface** — its own program and pass, drawn between the refraction resolve and the late pass, writing depth so everything sorts against it.
- **A spectral ocean** — a Tessendorf sea in 45 fragment ping-pong passes over three cascades, because GL 4.1 has no compute stage.
- **A projected grid** — a fixed lattice in NDC rather than a clipmap, so cell density is uniform in pixels and the surface reaches the horizon instead of stopping 5° short.
- **Shoaling against real terrain** — a baked bed field the waves shorten over, with a seabed under the sea and `apps/forest` able to flood.
- **Volume absorption** — depth-graded colour from per-channel extinction, with the submerged camera turning the body into a second froxel medium.
- **Caustics** — derived from the surface's own compression, on whatever it refracts.
- **Far-field filtering** — distant cells drop what sits under their footprint and hand the removed slope energy to roughness.
- **SSS profile tag on stencil** — moved off the alpha channel, where MSAA box-filtering corrupted it at partial coverage.
- **A first-person walker** — `apps/tree --player`, standing on the analytic surface, keyboard-only so the GUI stays clickable.

## v0.10.0 — 2026-08-12

- **A GPU profiler** — per-pass GPU time, CPU time and submission counts, in a HUD table and on stdout at exit.
- **Instanced submission** — repeated meshes batch through a std140 instance block, with LOD chains as index ranges in one EBO.
- **A walkable kilometre** — `apps/forest`, 5000 instanced props on 64 LOD'd tiles over heightfield terrain with a Jolt mesh collider.
- **The golden runner** — a stored-reference corpus, plus the gate suite that has policed every change since.
- **Moment-based OIT** — absorbance moments replace the McGuire depth curve, on by default.
- **Moment shadow maps** — the depth cascades resolve into filterable moments.
- **Translucent shadow maps** — translucent casters attenuate instead of blocking or vanishing.
- **Hair shading** — strand orientation and identity derived from the atlas, riding the anisotropy channel that already existed.
- **Lens flare and finishing** — ghosts off the bloom pyramid, chromatic aberration, vignette, grain, sharpen, grade.
- **Output dither** — a ±1 LSB triangular dither on the 8-bit write, on by default.
- **Draw order corrected** — the shadow catcher draws before the things that sort against it, and the opaque lane stops blending geometry that is not translucent.

## v0.9.0 — 2026-08-07

- **Volumetric clouds** — a shell-marched deck composited over the sky and into the environment bake, on threaded tiling noise.
- **TAAU** — render resolution splits from post resolution, reconstructed temporally at the seam.
- **Runtime resolution** — render targets rebuild in place at a new scale, resetting the temporal histories.
- **Bokeh depth of field** — per-tile CoC maxima set the gather radius, with a 64-tap N-gon gather that lets bokeh shapes read.
- **Pre-integrated skin** — a curvature-driven diffuse term, with a fixture ladder to shade it against.
- **Split ambient specular** — ambient diffuse and specular chosen independently, making specular occlusion exact by construction.
- **Bent-normal specular occlusion** — exported from the visibility bitmask GTAO already computes.
- **A fixed-step render clock** — one clock per frame read by every pass, so headless frame N is always state N.
- **Import unit scale** — a file's declared length unit normalised to metres, with escape hatches.

## v0.8.0 — 2026-08-02

- **A working-space contract** — the scene HDR buffer gets a stated meaning, and scale invariance holds under auto-exposure.
- **EV100 physical exposure** — an opt-in photometric camera, with exposure owned in one place instead of duplicated.
- **Photometric imports** — glTF light intensities pass through instead of being rescaled, with inverse-square falloff.
- **Area-light shadows** — area panels cast, with near-side depth and a projection fitted to the panel.
- **Stochastic PCSS** — the kernel rotates per pixel and frame while TAA can average it.
- **Receiver-plane bias** — one bias policy across cascades and punctual maps, biasing by the receiver's own plane.
- **Sheen conformance** — the Charlie kernel convolved into the environment, with lambda visibility and a squared alpha.
- **KHR specular conformance** — f90 scaling, achromatic trade, and the clamp order the spec requires.
- **SSR trace rewrite** — interval acceptance and honest occlusion in place of a point-sampled march.
- **Gates that can fail** — fixtures whose answer is known in advance, passing on peak error rather than a pixel budget.

## v0.7.0 — 2026-07-30

- **Clustered forward lighting** — a 16×8×24 frustum grid with std140 light UBOs, making the light count a non-issue.
- **LTC area lights** — rectangular area lights through the linearly-transformed-cosine quad integral.
- **Contact shadows** — a screen-space thickness march along the key light.
- **Froxel volumetric fog** — a 3D volume with temporal reprojection through its own previous camera, replacing the screen-space march.
- **Aerial perspective** — the atmosphere coupled to the scene through a per-frame volume, with a units-to-km knob.
- **Punctual shadow maps** — point, spot and area lights all cast, through one punctual array.
- **DDGI probe volume** — an octahedral irradiance atlas with a capture loop.
- **Photometric punctual lights** — intensity in stated units with inverse-square falloff.
- **Orthographic camera** — and 2D apps that no longer orbit.

## v0.6.0 — 2026-07-23

- **The `.cscn` scene format** — a JSON scene loader with a Blender exporter that preserves hand-authored keys.
- **A particle system** — SoA pools, composable modules and a pluggable renderer, with CPU and transform-feedback backends behind one vtable.
- **One main loop** — the engine owns the frame and `run_game` becomes a thin wrapper.
- **Dear ImGui** — replaces Nuklear, with input routed through it and the camera handoff fixed.
- **Vendored dependencies** — glfw, glew, cglm and assimp as pinned static submodules, with CMake presets.
- **Windows portability** — POSIX shims, a portable thread layer, and drive-absolute path handling.
- **Remote build orchestration** — `build.sh --target` for Linux and Windows VMs.
- **The procedural tree** — rebuilt as the engine's showcase, with grass, foliage clustering and directional wind.
- **A shader include seam** — `#include` at build time, with the shared chunks deduplicated behind it.
- **Async texture loading** — a worker pool decoding embedded textures off the main thread.

## v0.5.0 — 2026-07-20

- **HDR post stack** — MSAA resolve, bloom, tonemap and exposure as a real chain.
- **Cascaded shadow maps** — sphere-fit ortho cascades with PCF selection and per-cascade PCSS.
- **A physically-based sky** — Hillaire atmosphere LUTs feeding the skybox, IBL, probe and fog, with sun key-light coupling.
- **GTAO** — ground-truth ambient occlusion on the 2023 visibility bitmask, replacing SSAO.
- **Screen-space reflections** — hi-z traversal with a stochastic march and an à-trous denoise.
- **SSGI** — one-bounce indirect diffuse riding the GTAO sweep.
- **Volumetric fog** — height fog with shadowed in-scatter and temporal accumulation.
- **TAA** — sub-pixel jitter, camera and skinned-deformation motion vectors, and a resolve.
- **Subsurface scattering** — Jimenez diffuse separation, then a multi-Gaussian profile with per-material profiles.
- **Clearcoat, sheen and specular** — the KHR material extensions, analytic and IBL.
- **Parallax occlusion mapping** — with a dedicated height texture.
- **Bloom pyramid** — dual-filter downsample with tent upsample.
- **Motion blur** — McGuire velocity reconstruction.
- **AgX** — a display transform beside ACES and PBR Neutral.
- **Weighted-blended OIT** — order-independent transparency for alpha-blend materials.
- **Depth of field** — autofocus and a film-parameter path.
- **Spring bones** — procedural secondary motion on un-animated chains.
- **Shadow catcher** — a ground plane that receives shadow, with HDR light-lobe extraction aiming the key lights.
- **Headless mode** — hidden window, frame limit and screenshot capture, for CI and comparison.

## v0.4.0 — 2025-12-29

- **Skeletal animation** — bone matrices, playback, and a bind-pose recalculation path.
- **Cross-rig retargeting** — smart bone matching, then global-space retargeting with a source skeleton.
- **Bone X-ray** — a visualization mode showing bind pose against animated pose.
- **glTF conformance** — three phases: `doubleSided` and factors, `normalScale` and `aoStrength`, then vertex colours, UV1 and texture transforms.
- **Alpha masking** — for hair and foliage materials.
- **Embedded textures** — GLB and glTF textures decoded from the file.
- **CLI argument parsing** — the render app takes model, animation and skeleton paths.
- **Camera auto-configuration** — framing and movement speed derived from model size.

## v0.3.0 — 2024-07-12

- **The library splits out** — a static lib with apps beside it, replacing one monolithic build.
- **Materials and render passes** — moved into their own files, with materials owned by the scene and shared across meshes.
- **A shader program cache** — programs saved by name so the defaults can be referenced and reused.
- **Procedural geometry** — generated meshes with line shaders for non-filled shapes.
- **Linux builds** — and a build process that survives leaving macOS.
- **Input into the engine** — keyboard and mouse handling moves out of the app.

## v0.2.0 — 2023-12-09

- **Lights become first-class** — global positions that follow their nodes, and a fallback light when a scene ships none.
- **Light outlines** — a rectangle or sphere drawn around each light in the scene.
- **Watts to radiance** — the shader takes a physical quantity instead of a raw multiplier.
- **Camera modes** — free and orbit, toggled on the engine.
- **A texture pool** — textures loaded once and cached by path.
- **Back-face culling** — with a wireframe toggle in the GUI.
- **Per-material shader programs** — set on the material rather than the node.

## v0.1.0 — 2023-12-04

- **A scene graph** — an Engine orchestrating nodes, meshes and materials.
- **The first PBR pass** — physically-based shading on imported models.
- **A TBN basis** — tangents and bitangents computed and passed to the shaders.
- **4x MSAA** — with double buffering.
- **Texture caching** — a new `Texture` struct, loaded once.
- **Debug shaders** — selectable from a dropdown in the HUD.
