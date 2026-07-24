# Contact-shadow goldens (spec 9.3)

Screen-space contact shadows: an 8-step depth-buffer march toward the key light
that fills the fine contact gaps a cascaded shadow map is too coarse to resolve.
Off by default; `--contact-shadows` enables it.

All goldens are HDR-free: `--sky` bakes a procedural sun (a shadow-casting
directional, which is what the pass marches along), so nothing here needs the
gitignored studio HDRs and anyone can regenerate them (the 9.2 lesson).

## contact_debug.png

The raw visibility term (`--cs-debug`, debug view 8): 1 = lit (white),
0 = fully occluded (black).

```
./out/bin/render -m assets/area_light_fixture.gltf --no-scene-file \
    --sky --sun-elevation 15 --sun-azimuth 55 -W 640 -H 360 \
    --cam-eye 0,1.6,6 --cam-target 0,0.5,0 \
    -x -f 120 --no-auto-exposure -E 1.0 --cs-debug
```

What to look for: a sharp dark crescent on the anti-sun side of each sphere,
growing left to right (a perspective effect -- the camera at x=0 views each
sphere across the row at a different angle). No speckle, no full-hemisphere
darkening. **Inspect at 1:1** -- the earlier 9.2 golden shipped an artifact that
was invisible at thumbnail scale.

## The fixture under-sells this feature -- on purpose

A smooth sphere resting on a plane is a poor contact-shadow demo: its only
"contact" is the tangent line, and the visible term is mostly its own grazing
terminator. Contact shadows earn their keep on FINE geometry -- armor plate
seams, under a collar, between limbs -- exactly where a shadow map's texels are
too big. On the raiden model the term lands at the collar, armpits, hips and
knees and grounds the figure; that render is the real demonstration but needs
the gitignored HDR, so it is not committed here. The sphere fixture is the
reproducible regression check, not the showcase.

## Determinism

The march's start jitter freezes when TAA is off (the GTAO idiom), so headless
renders are byte-deterministic:

```
magick compare -metric AE run_a.ppm run_b.ppm null:   # must print 0
```

## Off / identity checks (run and compare, not stored)

- **Off-gate**: default (feature off) is byte-identical to the pre-9.3 build --
  the tonemap composite is an exact identity at cs == 1. Pin exposure
  (`--no-auto-exposure -E 1.0`) or auto-exposure drift masks it (the 9.2 note).
- **`--cs-distance 0` == off**: a C-side gate routes a zero-length march to the
  exact off path (0 px), rather than trusting GPU `mix()` identity.
- **`--no-shadows`**: no shadow-casting directional -> `fog_light_count` 0 ->
  the pass no-ops (0 px). The feature has no key-light language to extend
  without one; this is by design, documented in `--help`.
