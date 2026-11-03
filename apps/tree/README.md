# tree

A procedural tree generator (`tree_gen`) with ImGui sliders, standing on a domed island in a sea
with a seabed under it, at sunset. Every named preset -- broadleaf, dead, spruce, fir and snag -- is
in the panel's Preset combo, and each form has its own sliders: the recursive ones for broadleaf
and dead; crown, whorls and sprays for spruce, fir and snag. A conifer takes no season and drops no
leaves.

## Running it

```bash
./out/bin/tree
./out/bin/tree --preset spruce
./out/bin/tree --player                 # walk the island in first person
./out/bin/tree --no-water               # dry land: no sea and no seabed
```

The default camera is a mouse orbit. Under `--player`, WASD moves, the arrow keys turn the head,
Shift sprints and Space jumps; the mouse never moves the camera, so the sliders stay clickable.
G shows the GUI, X the axes, T wireframe.

Its full flag set, and two defaults that read as bugs when met cold (a sun 0.8 degrees up, which
leaves the frame dark and the shadows short of the island), are in `docs/cli-reference.md`,
"apps/tree".

## What it exercises

The tree generator and its wind, grass and falling leaves; the sea and its shore (spectral waves by
default), the sky at a low sun with stars and an exaggerated moon, and the diffraction glare, which
is on here where the engine leaves it off.

## Comparing frames

The orbit camera's drag damping reads the wall clock, so two headless runs of the orbit view are
not pixel-identical: measure the noise floor at your framing first. `--player --headless` does
not reach that path and is 0 px run to run, which makes it the framing for any A/B in this app.
`docs/verification.md` has the numbers.
