# LTC area-light goldens (spec 9.2)

Reference renders for `--area-light`. All use `assets/area_light_fixture.gltf`
(roughness sweep 0.05 -> 0.95, left to right, over a diffuse ground quad) with
an explicit camera and every other light source suppressed, so the panel is the
only thing lighting the frame:

- `-e <hdr> --no-key-light --ibl-intensity 0` gives ZERO analytic lights. Note
  the HDR is still required: without one, the app's "no IBL and no authored
  lights" path installs a three-point rig that would swamp the panel.
- `--no-ground` stops the skybox ground projection from filling the frame.
- `--cam-eye/--cam-target` pin the camera (the orbit default frames the row too
  far back to read).

## roughness_sweep.png

```
./out/bin/render -m assets/area_light_fixture.gltf \
    -e my_models/studio_small_03_8k.hdr -W 640 -H 360 -x -f 2 \
    --no-auto-exposure -E 1.0 --no-key-light --ibl-intensity 0 --no-ground \
    --no-scene-file --cam-eye 0,1.6,6 --cam-target 0,0.5,0 \
    --area-light 0,2.2,1.2,0,-0.6,-0.8,2.0,0.7,30
```

What to look for: the leftmost (roughness 0.05) sphere shows a small, sharp
reflection with recognisable rectangular structure and an otherwise dark body
(there is no environment to reflect); the highlight broadens smoothly rightward
into a near-uniform wash by 0.95. A rotated or smeared rectangle at low
roughness means the inverse-M reconstruction is transposed.

## backface_dark.png

Same command with the panel normal negated (`0,0.6,0.8`). Every sphere must be
pure black: a panel lights only the half-space its direction points into. If
anything is lit here, the single-sided plane test or the corner winding flipped.

## Energy checks (not stored as images -- run and compare)

Both cancel the tonemap exactly: matching linear radiance must give matching
pixels regardless of the curve. Compare the centre crop, since the sphere row's
horizontal spread makes off-axis distances scale differently from the centre:

```
magick <a>.ppm -gravity center -crop 300x300+0+0 +repage a.png   # same for b
magick compare -metric PAE a.png b.png null:
```

- **E1, inverse square** -- doubling distance while quadrupling radiance:
  `--area-light 0,8.5,0,0,-1,0,0.4,0.4,100` vs `0,16.5,0,0,-1,0,0.4,0.4,400`.
  Measured 2/255 PAE, 0.18% RMSE.
- **E2, radiance x area** -- same total power at fixed distance:
  `--area-light 0,8.5,0,0,-1,0,0.2,0.2,160` vs `0,8.5,0,0,-1,0,0.4,0.4,40`.
  Measured 1/255 PAE, 0.014% RMSE.

Regenerate the fixture with `python3 assets/gen_area_light_fixture.py`.
