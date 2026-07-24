# Contact-shadow goldens (spec 9.3)

Screen-space contact shadows: a short depth-buffer march toward the key light
that fills the near-contact darkening a cascaded shadow map is too coarse to
draw -- what grounds an object instead of letting it float. Off by default;
`--contact-shadows` enables it.

The demo asset is `assets/contact_fixture.gltf` (generator
`assets/gen_contact_fixture.py`): a matte plane with four matte cubes hovering
at increasing gaps above it (0.0, 0.06, 0.15, 0.30). It is self-lit -- the
render app's fallback key light casts shadows, so no `-e`/`--sky` is needed and
the fixture has no gitignored dependencies. Contact shadows darken the plane
under the low-gap cubes and fade as the gap grows -- the textbook "touching vs
floating" cue.

Why a purpose-built asset: contact shadows only read on MATTE receivers close
to an occluder. Chrome (raiden) has no diffuse term and its seams are already
dark; the LTC sphere fixture is glossy and tangent to the ground. Neither shows
anything. This fixture is built for the effect and is what exposed the original
thickness bug (see spec 9.3 as-built notes -- the merged version produced ~0
visible pixels on every scene).

## contact_debug.png

The raw visibility term (`--cs-debug`, debug view 8): 1 = lit (white),
0 = occluded (black).

```
./out/bin/render -m assets/contact_fixture.gltf --no-scene-file \
    --cam-eye 0,2.2,5 --cam-target 0,0.3,0 -W 640 -H 400 \
    -x -f 120 --no-auto-exposure -E 1.0 --cs-debug
```

What to look for: dark hugging the cube-ground contact of each cube, strongest
under the resting (gap-0) left cube and fading rightward. **Inspect at 1:1** --
the 9.2 golden shipped an artifact that was invisible at thumbnail scale.

Drop `--cs-debug` to see the composite (the cubes plant onto the plane);
`--cs-strength` / `--cs-distance` tune it.

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
  without one; documented in `--help`.
