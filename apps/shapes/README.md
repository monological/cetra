# shapes

Two filled red rectangles from `geometry.h`'s `mesh_generate_rect`, one with square corners and
one rounded, under an orthographic camera and the engine's 2D preset (`engine_set_2d_preset`), with
no light: under the preset a material's albedo is the colour on screen. It is the only user of
`PlaneDrag`, the plane camera in `app.h`.

```bash
./out/bin/shapes
```

A drag on a shape moves it, a drag on empty space pans, and the wheel zooms. G shows the debug GUI,
X the axes, T wireframe.

It keeps the engine's 4x MSAA and no TAA on purpose: multisampling is what 2D line art wants, and
nothing in the scene moves. `-x` and `-f N` run it headless for a number of frames, and
`--input-script` and `--trace-camera` are what the camera gate drives it with; it takes no
screenshot flag. See `docs/cli-reference.md`, "The other apps".
