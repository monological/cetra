# Procedural terrain

The terrain under `cetra/src/procedural/`: the heightfield, erosion, streaming, ground materials and the CDLOD quadtree. `apps/forest` is built on it.

## Modules

### `terrain.c/h`

fbm heightfield, tiled mesh + collider build, per-vertex slope/altitude tint. Since 11.59 the height has two SOURCES behind one pure function: the fbm, or a Catmull-Rom sample of a stored `TerrainField`. Since 11.60 `terrain_ground_classes` is the ONE statement of what the ground is made of — rock / silt / channel from slope and the erosion masks — read by the vertex tint, by `terrain_bake_splat` and (through the exported channel band) by the scatter, so the three cannot drift. They were three copies of four thresholds, and the two paint copies are never live in the same frame, so a drift would produce no contradiction to notice

### `erosion.c/h`

Mei virtual-pipes hydraulic + thermal erosion over a field (spec 11.59). CPU, Eulerian, double-buffered, threaded like the cloud noise bake — **bit-identical at any worker count**, which is the constraint that rules out droplet erosion. Produces the flow / deposit / wear masks, which are the point as much as the silhouette

### `terrain_stream.c/h`

The stored field paged off disk (spec 11.69, D4): an fp32 tiled `.cts` file and one rectangular WINDOW resident per pyramid level, anchored on the camera and the player. **Windows rather than 11.67's page table, and the choice is about ACCESS PATTERN not scale** — every heavy consumer walks a contiguous square and `sample_plane` runs ~30k times per patch build, so a 4-compare containment test beats a lookup per corner of a 4x4 footprint straddling up to four tiles; and a window's position is a pure function of the anchor path, which deletes the eviction ordering, want-guard and hysteresis rather than re-implementing them. The coarse tail stays whole, so a miss is ANSWERABLE: a query whose clamped footprint escapes a window retries one level coarser (twice the world each step) and gets exactly what that level returns unstreamed. **The file stores the pyramid already in memory, never re-filtered**, or that equality dies while level 0 stays green. **A window is `n*tile + 1` nodes** for the same reason a level is `2^k + 1`; sized `n*tile` the far edge of the world is unreachable and everything built there comes from the fall. Masks fall to a coarse plane in the file and that answer is an APPROXIMATION with no unstreamed twin — the height's is exact

### `terrain_tex.c/h`

The four ground materials a layered terrain blends between — grass, rock, silt, gravel (spec 11.60). Pure CPU, `vegetation_tex.h`'s malloc-and-return contract, and **every field is periodic**: a ground tiled across a kilometre from unbounded noise prints a hard grid at every boundary. Packed for the material texture array (albedo + height, normal + roughness + AO), because that packing is a contract with `layers.glsl` and a second place that knows it is a second place to get it wrong. Threaded on `cetra_bake_bands` like the cloud and erosion bakes, rows within a layer — `veg_noise_seed` rewrites a file static, so the seeding happens once before the split

### `terrain_quadtree.c/h`

A CDLOD quadtree over the terrain's domain (spec 11.63), replacing the fixed `tiles x tiles` grid: fine patches near the camera, coarse ones away, so the patch count tracks how many LEVELS there are and not how much ground they cover. Patches are CPU-built off `terrain_height_at` and cached — the alternative, one shared unit grid displaced by a height TEXTURE, makes the drawn surface a different function from the one the collider and the scatter agree through. **`TERRAIN_SPLIT_FACTOR` is a proof, not a knob**, and it is written down beside itself: two separate bounds (adjacent patches never more than one level apart, and a seam vertex at factor 1 from the fine side while the coarse side is still at 0) both require f ≥ 2√2
