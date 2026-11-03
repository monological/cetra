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

Installing a field changes where the number comes from and nothing else. Rules both sources keep:

- **A field's filter must be C1, so it samples bicubic, not bilinear.** `terrain_normal_at`
  central-differences the height over about 1.3 units on a cell of about 2, and a piecewise-constant
  derivative facets the normals and reaches the scatter's slope gate as rows of missing trees.
- **Outside `[-extent, +extent]` a field clamps its coordinate, not its taps.** Callers ask out
  there (the third-person eye trails the player off the domain); clamping the taps alone
  interpolates with an out-of-range `t` and extrapolates the cubic past the edge.
- **`terrain_height_at` is not thread-safe.** The analytic path memoizes its permutation table in
  file statics; the field path touches none, but the contract is the weaker of the two.
- **Island shaping applies to the analytic height alone** (`island_start`, `island_depth`, spec
  11.63; `island_start` outside (0,1) is off). `terrain_field_seed` writes the fbm into a field, so
  an eroded island is eroded as an island, while a field loaded from a file is left as the file
  says. The radius is Euclidean against a square domain, so the corners are open sea.
- **A height has levels.** `terrain_height_at_level(p, x, z, k)` is the surface with everything
  finer than level k's cell removed: a filtered mip on the field path, dropped octaves on the fbm
  path, and level 0 is `terrain_height_at` bit for bit. A consumer names a cell
  (`terrain_level_for_cell`), not a level, which keeps a quadtree patch and its parent one level
  apart without either knowing of the other. The pyramid is filtered under a separable `[1 2 1]`
  tent: taking every other node removes nothing whenever a patch cell is a whole multiple of a
  field cell.

## Erosion

`erosion.c` runs Mei's virtual-pipes hydraulic erosion and a thermal pass over a field (spec
11.59): on the CPU, Eulerian, double-buffered, threaded like the cloud noise bake.

- **It is bit-identical at any worker count, within one build.** That constraint is what rules out
  droplet erosion: two droplets landing in one cell resolve by arrival order, so no two thread
  counts agree. `--terrain-erosion-probe` prints an FNV-1a `digest` over all four planes, the only
  thing that can show it, since the budget sums hide compensating differences. An `-O0` and an
  `-O2` build of the same source differ.
- **Sediment moves by the fluxes**, in the pipes' own proportions. Mei's semi-Lagrangian gather
  leaks (3% of the sediment budget over 220 iterations), since a bilinear gather conserves mass only
  for a divergence-free field; by the fluxes the budget closes to rounding.
- **Rain over evaporation is the equilibrium water depth**, the one number to get right first.
  Set it to ten units over two-unit cells and the terrain is a lake: every cell drains into every
  other, flow comes out uniform and the mask paints the map one colour.
- **The flow, deposit and wear masks are as much the point as the silhouette.** A sim knows where
  water ran, so the ground can be gravel in a stream bed and bare rock on a scoured ridge.
  `terrain_mask_at` returns all three exactly 0 with no field, so a caller blending by them falls
  back to the un-eroded look with no special case. `flow` is drainage, not depth (rain falls on
  every cell, so standing water says nothing), log1p-compressed because drainage is heavy-tailed.
  Its mean is near 0.4: a channel is the top few per cent, and a threshold near the mean paints the
  whole map as riverbed.

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
- **A streamed field's planes address a resident window, not the whole level.**
  `TerrainField.stream` non-NULL means indexing the planes res by res is wrong, and only
  `terrain.c`'s samplers and `terrain_stream.c` may touch them; its height range comes from the
  file's manifest. `heightmap_save`, `terrain_field_measure` and `terrain_erode` do not check, so
  never hand them a streamed field. The sampler is the same either way.
- **The file stores the pyramid already in memory, never re-filtered.** Filtered again, the coarse
  answer stops equalling the unstreamed one while level 0 still matches.
- **A window is `n*tile + 1` nodes**, for the reason a level is `2^k + 1`: the grid is
  node-centred. Sized `n*tile`, the far edge of the world is unreachable and everything built there
  comes from the fall.
- **Only the height's fall is exact.** The masks fall to a coarse plane in the file, an
  approximation no unstreamed run reproduces. Anything that builds a persistent artifact (a
  collider, a scatter placement, a cached patch) calls `terrain_stream_ensure_rect` first.
