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
- Supersampling, order-independent transparency (weighted-blended, or
  moment-based for layered stacks), screen-space refraction
- Soft particles, shadow catcher, procedural skybox and HDR ground projection

### Lighting

- Directional, point, spot and rectangular area lights on photometric (EV100) intensities
- Cascaded and punctual shadow maps, PCF with optional stochastic PCSS contact hardening
- Rectangular area-light shading via linearly transformed cosines
- Split-sum IBL, from an HDR probe or baked from the sky
- Hillaire physically-based atmosphere, with aerial perspective and a volumetric cloud layer
- DDGI irradiance probe volume, parallax-corrected reflection probes, screen-space contact shadows

### Materials

- Metallic-roughness PBR with multi-scatter energy compensation
- glTF extensions: clearcoat, sheen (Charlie, with an environment prefilter), specular, transmission / IOR, volume / thickness
- Subsurface scattering: pre-integrated skin diffuse over a screen-space scatter pyramid
- Parallax occlusion mapping, anisotropy, thin film, vertex colors, a second UV set
- 13 texture slots, scalar masks packed into a texture array, pooled loads and async streaming

### Post-processing

- GTAO with bent normals and split specular occlusion; screen-space GI
- Screen-space reflections: hi-z trace, temporal accumulation, à-trous denoise
- Froxel volumetric fog (god rays, height haze) and aerial perspective
- Bokeh depth of field, McGuire motion blur, bloom pyramid
- Auto-exposure, ACES / AgX / neutral tonemaps, film finish (grain, vignette, sharpen, color grade)

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
```

`render --help` lists the full set of rendering, lighting and post-processing
switches.
