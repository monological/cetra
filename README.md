<div align="center">

<pre>
┏┓┏┓┏┳┓┳┓┏┓
┃ ┣  ┃ ┣┫┣┫
┗┛┗┛ ┻ ┛┗┛┗
</pre>

<h3>
    Cetra Graphics Engine
</h3>

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
- Front-to-back opaque ordering, optional depth prepass, frustum culling
- Soft particles, shadow catcher, procedural skybox and HDR ground projection

### Lighting

- Directional, point, spot and rectangular area lights on photometric (EV100) intensities
- Cascaded and punctual shadow maps, PCF with optional stochastic PCSS contact hardening
- Moment shadow maps, and translucent casters that attenuate instead of blocking
- Rectangular area-light shading via linearly transformed cosines
- Split-sum IBL, from an HDR probe or baked from the sky
- Hillaire physically-based atmosphere, with aerial perspective and a volumetric cloud layer
- Cloud shadows: a sun-transmittance map through the deck, reaching the ground, the fog and
  the water
- DDGI irradiance probe volume, parallax-corrected reflection probes, screen-space contact shadows

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

### Materials

- Metallic-roughness PBR with multi-scatter energy compensation
- glTF extensions: clearcoat, sheen (Charlie, with an environment prefilter), specular, transmission / IOR, volume / thickness
- Subsurface scattering: pre-integrated skin diffuse over a screen-space scatter pyramid
- Anisotropy, and hair with strand orientation and identity riding the same channel
- Parallax occlusion mapping, thin film, vertex colors, a second UV set
- 13 texture slots, scalar masks packed into a texture array, pooled loads and async streaming
- Procedural generators for terrain, sand, rock, foliage and trees, with histogram-preserving
  stochastic sampling so a tiled texture stops reading as tiled

### Post-processing

- GTAO with bent normals and split specular occlusion; screen-space GI
- Screen-space reflections: hi-z trace, temporal accumulation, à-trous denoise
- Froxel volumetric fog (god rays, height haze) with local fog volumes, and aerial perspective
- Bokeh depth of field, McGuire motion blur, bloom pyramid, lens flare
- Auto-exposure, ACES / AgX / neutral tonemaps, film finish (grain, vignette, chromatic
  aberration, sharpen, color grade, output dither)

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
