# forest

A walkable island: thousands of instanced trees and rocks on a CDLOD terrain quadtree, with the
props and their collision made resident per region round the player, a sea past the shore, a
gravel trail, wind in the trees, and a character on a Jolt mesh collider. It is built so that
instancing, LOD and frustum culling all matter at once: a few prototype meshes shared by thousands
of nodes, grouped so the batcher can join them, and terrain cut into patches that are each the unit
of culling and of LOD.

## Running it

```bash
./out/bin/forest
./out/bin/forest -x -f 60 -S /tmp/forest.ppm --profiler
```

WASD or the left stick walks, Shift sprints, Space jumps, and the mouse, the right stick or the
arrows look. A click captures the cursor and Tab releases it. V switches between first person and
an orbit behind the player.

Flags to start with:
- `--terrain-extent <f>`: the domain's half-width, which grows the island past a kilometre.
- `--no-island`: the flat domain with no sea.
- `--no-instancing`, `--no-lod`: one draw per mesh, or every draw at LOD 0.
- `--walk <speed>`: drives the character forward headless, the only way a headless run crosses a
  region boundary.

The full set -- the quadtree, regions, the trail, erosion, a streamed terrain, origin shifting and
the cook -- and two things about the island that read as bugs (sea level is -35, and the island is
a low disc) are in `docs/cli-reference.md`, "apps/forest".

## What it exercises

The draw list's batching, LOD and culling under real content; `procedural/` (terrain, erosion, the
quadtree, streaming); wind on cullable vegetation; the water on real terrain; and the game layer's
character controller on a mesh collider.

Its frames are not pixel-identical run to run: the sky is the source, and `--no-sky` or
`--render-mode 6` is what to compare for a 0 px claim. Geometry, scatter, LOD and physics are exact,
and the `forest-*` gate arms read those integers. `docs/verification.md` has the numbers.
