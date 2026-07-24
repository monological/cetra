# Contact-shadow goldens (spec 9.3, rebuilt in 9.4)

Screen-space contact shadows: a short depth-buffer march toward the key light
that fills the near-contact darkening a cascaded shadow map's texels are too
coarse to draw. Off by default; `--contact-shadows` enables it.

**What the feature actually does (9.4):** it is a SHORT-RANGE supplement to the
shadow map. The big shadow an object casts on the ground is the CSM's job (a large
depth delta); the canonical march keeps only the SMALL-delta near-contact and
rejects the rest. So a lone primitive on a plane shows almost nothing. The feature
reads on fine near-contact DETAIL -- many surfaces close together below a
shadow-map texel.

The demo asset is `assets/contact_fixture.gltf` (generator
`assets/gen_contact_fixture.py`): a seeded mound of 70 matte rocks, so the frame
is full of rock-to-rock and rock-to-ground crevices -- each a genuine small-delta
contact the CSM blurs over. Its sibling `assets/contact_fixture.cscn` ships a
procedural-sky environment (ambient the crevices need so the term has something to
darken) with an authored sun angle, plus the camera, so the fixture is fully
self-contained -- no `-e`/`--sky`, no camera flags.

Why a rock pile, not the old cubes: a cube fills the frame with hard silhouette
EDGES, and a march grazing its own edge self-shadows into a streak -- the worst
case, and it demonstrates nothing the shadow map doesn't already do. Curved rocks
don't self-graze, and a heap of them is all near-contact. (The earlier cube
fixture is what exposed both the original thickness bug and, later, the streak the
coherence gate left behind -- see spec 9.4.)

## contact_debug.png

The raw visibility term (`--cs-debug`, debug view 8): 1 = lit (white),
0 = occluded (black).

```
./out/bin/render -m assets/contact_fixture.gltf -W 640 -H 400 \
    -x -f 120 --no-auto-exposure -E 1.0 --cs-debug
```
(The sibling `.cscn` supplies the sky, sun, and camera; `--no-scene-file` would
drop them and is exactly what you do NOT want here.)

What to look for: dark hugging the crevices between and beneath the rocks, and
none on the open lit surfaces or the bare ground. **Inspect at 1:1.**

Drop `--cs-debug` to see the composite (the crevices deepen, the rocks read as a
pile instead of floating apart); `--cs-strength` / `--cs-distance` tune it.

## Determinism

The march's start jitter freezes when TAA is off (the GTAO idiom), and the sky
bakes once for a static sun, so headless renders are byte-deterministic:

```
magick compare -metric AE run_a.ppm run_b.ppm null:   # must print 0
```

## Off / identity checks (run and compare, not stored)

- **Off-gate**: default (feature off) is byte-identical to the pre-9.3 build --
  the tonemap composite is an exact identity at cs == 1. Pin exposure
  (`--no-auto-exposure -E 1.0`) or auto-exposure drift masks it.
- **`--cs-distance 0` == off**: a C-side gate routes a zero-length march to the
  exact off path (0 px), rather than trusting GPU `mix()` identity.
- **`--no-shadows`**: no shadow-casting directional -> the pass no-ops (0 px). The
  feature has no key-light language to extend without one.
