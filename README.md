<div align="center">

<pre>
┏┓┏┓┏┳┓┳┓┏┓
┃ ┣  ┃ ┣┫┣┫
┗┛┗┛ ┻ ┛┗┛┗
</pre>

<h3>
    Cetra Graphics Engine
</h3>

[![release](https://img.shields.io/github/v/tag/monological/cetra?sort=semver&label=release&color=5A56E0)](CHANGELOG.md)

</div>


- Written in C11, on OpenGL 4.1 core.
- No system dependencies — glfw, glew, cglm, assimp, Jolt and Dear ImGui are vendored and built from source.
- Forward renderer with an HDR G-buffer and a full screen-space post stack.
- macOS, Linux and Windows.

---

[raiden.mp4](https://github.com/user-attachments/assets/0aa7ccea-ed15-4b0b-b408-aed6c6c3c8cd)

---

## Features

### Rendering

- Forward renderer, HDR + MSAA, multi-target G-buffer (color / normals / motion / albedo / SSS)
- Clustered forward light culling on a 16x8x24 frustum grid, up to 64 lights per draw
- Temporal AA, plus TAAU render-scale upscaling with runtime resolution switching
- Supersampling, moment-based order-independent transparency, screen-space
  refraction
- Instanced submission through a std140 instance block, with meshoptimizer LOD chains as
  index ranges in one buffer
- Cluster-DAG LOD: ~128-triangle clusters grouped and simplified with their boundaries locked,
  every level indexing the original vertex buffer, so cracks are structurally impossible
- Front-to-back opaque ordering, optional depth prepass, frustum culling — including conservative
  bounds for wind and skinning, which displace vertices past the mesh's authored box
- Clustered decals projected through an oriented box, selected by a per-froxel mask
- Soft particles, shadow catcher, procedural skybox and HDR ground projection

### Lighting

- Directional, point, spot and rectangular area lights on photometric (EV100) intensities
- IES photometric profiles: a real luminaire's measured 2D distribution, replacing a spot's
  analytic cone rather than multiplying it
- Emissive geometry derived as LTC area panels, by fitting a rectangle to the mesh
- Cascaded and punctual shadow maps, PCF with optional stochastic PCSS contact hardening
- Moment shadow maps, and translucent casters that attenuate instead of blocking
- Rectangular area-light shading via linearly transformed cosines
- Split-sum IBL, from an HDR probe or baked from the sky
- Hillaire physically-based atmosphere, with aerial perspective and a volumetric cloud layer
- Cloud shadows: a sun-transmittance map through the deck, reaching the ground, the fog and
  the water
- DDGI irradiance probe volume; up to eight parallax-corrected reflection probes, blended per
  fragment off a per-froxel mask
- Screen-space contact shadows, marching the key light and every local light with no shadow map

### Water

- Tessendorf spectral ocean over three cascades, or a cheaper Gerstner surface
- The FFT runs as fragment ping-pong — GL 4.1 has no compute stage, and a Stockham butterfly
  is a pure gather, so it costs draws rather than a redesign
- Two summed directional wave trains, wind sea and swell, each with its own wind speed and
  fetch on a JONSWAP/TMA spectrum
- A projected grid in NDC rather than a clipmap, so cell density is uniform in pixels and the
  surface reaches the horizon
- Shoaling against a bed field, with depth-limited breaking and a traced shoreline driving the
  swash
- Foam in three bands — crest from the Jacobian, shore from the shoal factor, whitecaps from
  the breaking face — accumulated per cascade texel so it outlives the wave that made it
- Volume absorption from per-channel extinction, plus caustics derived from the surface's own
  compression
- A submerged camera turns the body into a second froxel medium
- Cox-Munk sun glitter, wind-anisotropic, with far-field roughness derived from the spectrum's
  own slope variance
- A CPU query of the Gerstner surface, so gameplay and physics can stand on the water

### Terrain

- A CDLOD quadtree: fine patches near the camera and coarse ones away, so the patch count tracks
  how many levels there are rather than how much ground they cover
- Patches are CPU-built off the height function the collider and the scatter agree through, rather
  than a shared grid displaced by a height texture, which would be a different surface
- Height from a runtime fBm or a stored field behind one sampler, with a filtered mip pyramid, and
  a 16-bit heightmap on either side of it
- Mei virtual-pipes hydraulic and thermal erosion, threaded and bit-identical at any worker count,
  producing flow / deposit / wear masks as much as a silhouette
- Layered materials: N layers blended per texel from a splat, height-weighted so gravel interlocks
  with sand, on a world-aligned triplanar projection
- Roads as splat-weight overrides, applied before the height blend so a road is made of a layer
  instead of painted over one
- A composite cache over the blend, plus a paged virtual texture whose residency is driven by
  frustum prediction and a GPU feedback pass
- Streaming: an fp32 tiled pyramid on disk with one rectangular window resident per level, so
  growing a world is free and only refining it costs
- A walkable island with props and collision resident per region around the player

### Materials

- Metallic-roughness PBR with multi-scatter energy compensation
- glTF extensions: clearcoat, sheen (Charlie, with an environment prefilter), specular, transmission / IOR, volume / thickness
- Subsurface scattering: pre-integrated skin diffuse over a screen-space scatter pyramid
- Anisotropy, and hair with strand orientation and identity riding the same channel
- Parallax occlusion mapping, thin film, vertex colors, a second UV set
- 13 texture slots, plus a scene-wide array of unique per-texel images — masks, material layers
  and decals — so a whole feature costs a layer index rather than a sampler declaration
- Pooled loads and async streaming
- Procedural generators for terrain, sand, rock, foliage and trees, with histogram-preserving
  stochastic sampling so a tiled texture stops reading as tiled

### Post-processing

- GTAO on the visibility bitmask, with specular occlusion read from the same sector mask and a
  depth-aware bilateral upsample off half res; screen-space GI
- Screen-space reflections: hi-z trace, temporal accumulation, à-trous denoise
- Froxel volumetric fog (god rays, height haze) with local fog volumes, and aerial perspective
- Bokeh depth of field, McGuire motion blur, bloom pyramid, lens flare
- Histogram auto-exposure with uniform / centre-weighted / spot metering and percentile tails
- ACES / AgX / neutral tonemaps, 3D LUT colour grading from a `.cube` table with tetrahedral
  interpolation, film finish (grain, vignette, chromatic aberration, sharpen, grade, output dither)

### Animation

- Skeletal animation, GPU skinning up to 128 bones, prev-pose motion vectors
- Cross-rig retargeting by semantic bone matching
- Verlet spring bones for secondary motion
- Directional wind driving foliage, grass and cloth

### Systems

- Scene graph with hierarchical transforms; lights, cameras and particle systems are scene citizens
- Particle system: composable spawn/init/update modules, CPU and transform-feedback backends, curl noise, colliders
- Optional game framework: fixed-timestep loop, Jolt physics, character controller, ECS-lite entities
- `.cscn` scene format with a Blender exporter that bakes material graphs glTF cannot carry
- A JSON config snapshot: ~230 tuned settings dumped and restored, so a session can be handed to
  someone else instead of described
- A world origin shift, for worlds too large to measure from one point in fp32
- FBX / glTF / GLB / OBJ import, SDF text rendering, Dear ImGui debug panels
- GPU profiler: per-pass GPU and CPU time with submission counts, in a HUD table and on stdout
- A golden-image corpus and a gate suite of fixtures whose answer is known in advance

---

## Setup

Dependencies are vendored as submodules and built from source, so there is
nothing to install but a toolchain.

```
git clone --recursive https://github.com/monological/cetra
```

### On MacOS:

```
brew install cmake ninja
```

### On Ubuntu:

```
sudo apt-get install clang cmake ninja-build zlib1g-dev xorg-dev \
    libwayland-dev libxkbcommon-dev wayland-protocols extra-cmake-modules
```

### On Windows:

Visual Studio Build Tools (for the Windows SDK) with clang-cl, plus CMake and
Ninja. Configure from an x64 Native Tools prompt.

## Build

```
./build.sh              # debug   -> out/bin
./build.sh --release    # release -> out/release/bin
./build.sh --clean      # reconfigure from scratch
```

On Windows, from an x64 Native Tools prompt:

```
cmake --workflow --preset windows-debug
```

## Run

Example apps are built in the `out/bin` directory.

```
./out/bin/render -m assets/c64.fbx -t assets/c64.fbm
./out/bin/tree --player
./out/bin/forest
```

`render --help` lists the full set of rendering, lighting and post-processing
switches.
