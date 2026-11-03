# render

The engine's model and scene viewer: an FBX, glTF/GLB or OBJ model, or a `.cscn` scene file, under
an orbit camera, with HDR image-based lighting, skeletal animation and cross-rig retargeting. It
is also the engine's test bed: every golden in `assets/goldens/` is rendered by it, and most gate
arms drive it.

## Running it

```bash
./out/bin/render -m assets/scenes/cornell_rooms.cscn
./out/bin/render -m model.glb -e env.hdr -a clip.fbx
./out/bin/render -m model.glb -x -f 60 -S /tmp/out.ppm    # headless capture
```

`-m` is required. Drag orbits, and G shows the debug GUI, X the axes, T wireframe, and 1 to 6 the
render modes. Windowed runs have TAA on; headless runs have it off unless `--taa`.

Its flags, the PostFX and environment set and the headless capture set, are in
`docs/cli-reference.md` ("PostFX and environment flags" and "Headless Mode"). A session's GUI
state can be dumped and restored with `--config-dump` and `--config`; see `docs/verification.md`.
