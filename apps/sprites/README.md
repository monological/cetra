# sprites

A 2023 particle-globe sketch on cetra, behaving as the sketch did: 540 hard-square points on a small
sphere, each with a vertical jitter that picks a new target every second, coloured at random every
frame and sized by depth, turning faster and faster about the vertical axis. The points are one
particle emitter on the billboard renderer, with the placement and the motion in modules of the
app's own. It runs under the 2D preset with the passthrough tone curve, so its raw colours reach
the screen.

```bash
./out/bin/sprites
./out/bin/sprites -x -f 120 -S /tmp/sprites.ppm
```

`-x` hides the window, `-f N` exits after N frames, `-S PATH` writes the last frame as a binary PPM.
It keeps four MSAA samples: its particles write no motion vector, so TAA would smear them.
