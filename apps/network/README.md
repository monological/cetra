# network

A 2024 wireframe-sphere sketch on cetra: a rotating sphere from the engine's generator, its
triangles reduced to unique edges and drawn as lines, shaded by the sketch's own two shaders on a
material, whose colour is a sine of time and screen position. The renderer sets `model`, `view` and
`projection` on any program that declares them; the app sets `time` each frame. It runs under the
2D preset and keeps four MSAA samples, which line art wants.

```bash
./out/bin/network
./out/bin/network -x -f 120 -S /tmp/network.ppm
```

`-x` hides the window, `-f N` exits after N frames, `-S PATH` writes the last frame as a binary PPM.
