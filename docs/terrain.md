# Procedural terrain

The terrain under `cetra/src/procedural/`: a heightfield, the erosion that shapes it, the files it
is saved to and streamed from, the ground materials it is painted with, and the CDLOD quadtree that
draws it. `apps/forest` is built on it.

## The height is one pure function

`terrain_height_at(params, x, z)` is the surface. The drawn mesh, the physics collider and the
scatter all ask it, and none keeps a grid of its own, which is what keeps the three in agreement.
`terrain.c` builds the tiled mesh and collider from it and gives each vertex a tint from slope and
altitude.

The height has two sources behind that one function (spec 11.59):

- **The fbm**, over a private Perlin table, defined everywhere.
- **A stored `TerrainField`**, sampled Catmull-Rom, which clamps to its edge row outside the domain.

Installing a field changes where the number comes from and nothing else.

## Erosion

`erosion.c` runs Mei's virtual-pipes hydraulic erosion and a thermal pass over a field (spec
11.59): on the CPU, Eulerian, double-buffered, threaded like the cloud noise bake.

- **It is bit-identical at any worker count.** That constraint is what rules out droplet erosion:
  two droplets landing in one cell resolve by arrival order, so no two thread counts agree.
- **The flow, deposit and wear masks are as much the point as the silhouette.** A sim knows where
  water ran, so the ground can be gravel in a stream bed and bare rock on a scoured ridge.

## Heightmap files

`heightmap.c` reads and writes `.r16` and reads 16-bit PNG, so a field can be baked and eroded once
and loaded with no sim, or come from another tool. It does not go through `texture.c`, whose format
table cannot ask for 16-bit at all.

## What the ground is made of

`terrain_ground_classes` (in `terrain.c`) is the one statement of the ground's classes, rock, silt
and channel, from slope and the erosion masks (spec 11.60). The vertex tint, `terrain_bake_splat`
and the scatter (through the exported channel band, `TERRAIN_CHANNEL_FLOW_LO`/`_HI`) all read it.
Keep it one: the tint and the splat are never live in the same frame, so two copies that drifted
apart would show no contradiction to notice.

`terrain_tex.c` makes the four ground materials a layered terrain blends between: grass, rock, silt
and gravel (spec 11.60).

- **Pure CPU**, under `vegetation_tex.h`'s malloc-and-return contract.
- **Every field is periodic.** A ground tiled across a kilometre from unbounded noise prints a hard
  grid at every tile boundary.
- **Packed for the material texture array**: albedo + height in one layer, normal + roughness + AO
  in the other. The packing is a contract with `layers.glsl`, so it lives in one place.
- **Threaded on `cetra_bake_bands`**, rows within a layer. `veg_noise_seed` rewrites a file static,
  so the seeding happens once, before the split.

## The quadtree

`terrain_quadtree.c` is a CDLOD quadtree over the terrain's domain (spec 11.63), in place of a
fixed `tiles x tiles` grid: fine patches near the camera, coarse ones away, so the patch count
follows how many levels there are and not how much ground they cover.

- **Patches are built on the CPU from `terrain_height_at` and cached.** The textbook alternative,
  one shared grid displaced by a height texture in the vertex shader, draws a different surface
  from the one the collider and the scatter agree on.
- **`TERRAIN_SPLIT_FACTOR` is a proof, not a knob.** Two separate bounds, adjacent patches never
  more than one level apart and a seam vertex fully morphed on the fine side while the coarse side
  has not begun, each need f >= 2√2. The derivation is written beside the constant.
- **The morph to a parent's surface rides two vertex attributes**, `GL_ATTR_MORPH` and
  `GL_ATTR_MORPH_NORMAL`, and is a displacer with all a displacer's obligations
  (`docs/shaders.md`, Vertex Attributes).

## Streaming

`terrain_stream.c` pages a stored field off disk (spec 11.69): an fp32 tiled `.cts` file, and one
rectangular window resident per pyramid level, the coarse levels aimed at the camera and level 0 at
the player. The coarse tail stays whole, so a miss has an answer: a query whose footprint escapes a
window retries one level coarser, covering twice the world each step, and gets exactly what that
level returns unstreamed.

- **Windows, not a page table, because of the access pattern.** Every heavy consumer walks a
  contiguous square, and `sample_plane` runs about 30k times per patch build, so a four-compare
  containment test beats a lookup per corner of a 4x4 footprint straddling up to four tiles. A
  window's position is also a pure function of the anchor path, so there is no eviction order or
  hysteresis to get wrong.
- **The file stores the pyramid already in memory, never re-filtered.** Filtered again, the coarse
  answer stops equalling the unstreamed one while level 0 still matches.
- **A window is `n*tile + 1` nodes**, for the reason a level is `2^k + 1`: the grid is
  node-centred. Sized `n*tile`, the far edge of the world is unreachable and everything built there
  comes from the fall.
- **Only the height's fall is exact.** The masks fall to a coarse plane in the file, an
  approximation no unstreamed run reproduces. Anything that builds a persistent artifact (a
  collider, a scatter placement, a cached patch) calls `terrain_stream_ensure_rect` first.
