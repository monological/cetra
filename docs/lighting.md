# Lighting environment

Shadows, cached shadows, GI volumes, IBL, rain, the sky, reflection probes and the derived area panels, and the captures they share.

## Lighting Environment

- **Shadows:** cascaded shadow maps (up to 3 cascades, up to 3 shadow-casting lights
  into a `GL_TEXTURE_2D_ARRAY`), per-cascade bounding-sphere ortho fitting, 3x3 PCF by
  default with optional PCSS contact-hardening. Stored on `scene->shadow_system`.
  **A cascade's soft-shadow thresholds are counted in texels of its map, never as a fraction of
  its depth range** (spec 13.58). The range is mostly the scene pad its eye is pushed back by,
  about 4% of the scene's radius, and a fraction of it dropped every occluder shorter than that;
  see `docs/shader-subsystems.md`, Cascade soft shadows. The
  per-frame PUNCTUAL pool is sixteen layers of one array: a spot takes one, a point light six,
  and an area light FIVE (spec 13.27) -- a cube in the panel's own frame, its width axis, its
  up and its normal standing for x, y and z, with the face behind the panel never drawn, since
  the LTC answers zero there. Its faces end at the panel's RANGE rather than the scene's far
  plane: at the far plane a tube's side faces took in the whole street, 14.4 ms of GPU where
  the range costs 0.96. **One 120-degree map down the normal is what it replaced**, and that
  map read every receiver more than 60 degrees off the normal as lit, so a panel lit the rooms
  round it through their walls. A light takes all its layers or none, and past a light the pool
  cannot fit, a later one needing fewer may still fit. The cap is the driver's uniform limit, not GL 4.1's
  guaranteed 1024 components: the uber program already used 1029 at eight layers.
  **The array is laid out for the frame's per-frame layers, and a capture keeps that layout**
  (spec 13.44): one layer is drawn at 4096, more at less as the budget allows, none at 1024, and
  the cached tiles are laid out for that edge. The frame counts its layers before anything
  captures (`shadow_punctual_frame_update`), and every pass of the frame lays the array out for
  at least those. A capture holds the scene at rest, where a light may stop casting -- silent
  puts its flashlight away -- and an array laid out for the capture's fewer layers was rebuilt in
  the capture and again in the frame, every frame a capture ran, carrying every kept tile across
  both times. A light still casting at rest is captured at the frame's edge, so the capture key
  folds it (`shadow_punctual_edge_drawn`).
- **Cached point- and area-light shadows** (specs 13.16 and 13.27): a point light or a panel
  with `cast_shadows`, `shadow_cache` and a range keeps its faces as 256^2 TILES of the punctual
  array, past the per-frame layers and the rain's, drawn once and kept, under a 192 MiB budget
  of their own -- which is what lets the candles of silent's two houses, its hall lantern and
  its kitchen tubes cast where the per-frame pool stops at sixteen layers. **A panel is cached
  from its CENTRE over the six world-axis faces a point light's are**, one view and no body,
  so it is the per-frame cube's hard shadow at a tile's 256^2. A cached light's radius rides
  `colorIntensity.w` and its near plane `upArea.w`, slots no type reads for anything else; both
  used to take a panel's size from the slots the LTC reads it in. **Since 13.24 the budget goes to the NEAREST lights**: the cached
  lights are ranked by distance from the camera to their reach once a frame, a nearer light
  takes the block of the farthest holder more than 2 m farther, and once the tiles have opened
  at most `tile_new_blocks_per_frame` (2) blocks are placed a frame, since a light placed draws
  every face at once. A light without a block is unshadowed, so a GI or probe capture waits
  until every cached light reaching its box holds a whole one (`shadow_tiles_cover`). Seven
  things about it are easy to get backwards.
  - **A change to the scene graph draws again only the faces it touches** (spec 13.38,
    `tiles_mark_graph_change`).
    - The draw list's items are matched to the faces' record by NODE SERIAL and MESH ID, never
      by place in the list (one insertion shifts every later index) and never by pointer (a node
      freed and another created can share an address).
    - An item gone marks the box it was drawn in, which the record keeps current as its node
      moves; an item new marks where it is now; an item held but uploaded again, or drawn
      otherwise, marks both.
    - Every face is drawn again only when it cannot answer: memory, a caster with no bound that
      is gone, new or changed, or the first frame after one with no note at all.
    - It used to be every face on any add or remove, which is what turned silent's woods,
      hanging trees as the player walked, into 650 ms frames (13.35). `tiles-graph-*` hold it,
      the render app's `--graph-churn` and `--graph-replace` reaching it.
  - **A light with a body is drawn from 8 VIEWS spread over it** (`source_radius`,
    `source_length`: a capsule whose length is BETWEEN its caps), and its soft shadow is their
    AVERAGE, each view's edge blurred under TAA to meet its neighbours'. One cube map knows the
    nearest surface from ONE point and cannot tell a point it cannot see from a solid one: a
    march through it (Unreal's SMRT, which this spec built first) puts a candle's full shadow
    at the size of its CENTRE'S hard shadow, measured 18.6 cm against a traced 12.5. The
    `tile_core_fixture` and its traced truth are what `tiles-truth` holds this to.
  - **The flame's flicker is a READ, not a redraw**: the kept views are read where the body
    has moved them to, its centre's move times (dR - dB) / dB, so no face is drawn again.
  - **A caster that moves is drawn over a copy, not with the room**: a face that sees a MOVER
    keeps its still casters in a STORE CELL and is that copy plus the movers each frame, which
    is Unreal's static/dynamic shadow caching. A mover still for 120 frames becomes still again.
    **The cells are a pool of 64 ON TOP of the budget** (spec 13.26), a cell a face, to faces
    drawn this frame, nearest light first: inside the budget the lights took every cell first,
    so silent's sixteen bodied lights left no store and every such face was drawn whole, every
    caster in it, every frame. A face keeps its cell while nothing moves in it, until a face on
    screen needs it. `--tile-stores 0` is that path, and a face left without a cell is said by
    name each time the CAMERA's pass runs out (spec 13.32): a capture's pass draws every face a
    mover reaches, in view or not, and runs out where the camera's does not, so the two sharing
    the warning said it every frame a capture ran.
  - **A swaying mesh moves only under a wind that moves it, and its material may keep it at
    rest** (spec 13.26). A wind response under no wind, or a wind of no strength, is still. And
    `cachedShadowWind: "rest"` (Unreal's "Rigid") holds a swaying mesh in the kept faces at its
    rest pose, drawn there with no wind response, while the camera, the cascades and the
    per-frame lights keep the sway. A material change the graph cannot see redraws the faces of
    the caster it changed, not every face. silent's 110 dead trees made every face they reached
    a mover on every frame: 408 faces and 49 million triangles a frame, 176 ms of a 227 ms
    frame, following a sway of under a millimetre.
  - **Glass casts nothing for a cached light** (the kept caster sets leave the blend and
    transmissive lanes out): drawn solid, it put the hall clock's dial in shadow behind its own
    door.
  - **A light with no body is read from exactly where its faces were drawn** (spec 13.31), so
    two things follow. Moving, its shadow STEPS each time it passes `tile_tolerance`, unless it
    sets `shadow_follow`, which draws its faces again at any move -- silent's swinging bulb, while
    its swing is wide. And its lookup allows for float ROUNDING as well as the map's quantization
    (`TILE_ROUNDINGS`): a face is drawn through a matrix carrying the light's world position, so
    its depths round at that size, while the lookup projects `worldPos - centre`, which does not.
    Half a metre from a light whose near plane is 5 cm, 19 m from the origin, one rounding is
    seven ULPs of the map against a floor of four, and a surface squarely facing the light -- one
    depth across the whole face -- passed or failed its own test all at once: under a moving light,
    a hard rectangle bounded by the face's edges came and went every few frames. The per-frame
    lookup reads through the matrix it drew with, its roundings largely cancel, and it has no such
    allowance.
  `--tile-reference M` draws every cached light from M views of its body as it is THIS frame,
  redrawn each frame and averaged with no blur: the shadow by its definition, which is how the
  kept views are checked.
- **GI volumes** (DDGI, spec 9.7; a world list since 13.24, `gi_volume.h`): any number of
  grids, one per place, the nearest eight RESIDENT in slots of the scene's lighting atlas.
  A fragment reads the volume it stands in, or the nearest swept one's clamped edge just past
  it, FADING TO THE ENVIRONMENT over three of that volume's cells (spec 13.25) -- and the
  environment inside one still sweeping. A grid's edge probes say nothing about ground far
  off, where every probe fails the visibility test: with the edge carried out across silent's
  hillside the weights underflowed to an exact zero and laid black lines along the box's
  planes. Every sweep, the opening one at load included, takes probes nearest volume first
  under one capture burst while the frame's CAPTURE BUDGET allows (`Engine.capture_budget_ms`,
  spec 13.32), and a re-convergence no more than the world's `rate` besides. A slot is read
  back and KEPT when it is evicted, so leaving residency costs an upload to come back from;
  one evicted mid-sweep keeps nothing, and sweeps again from a CLEARED slot, since a volume's
  tiles do not cover its whole rectangle and the last holder's would show between them.
  `create_gi_volume_spaced` lays a
  grid over a box with `classify` on: each probe also captures its back faces, DEPTH ONLY
  (spec 13.32: the shaded second capture was half of a sweep), and one that finds a back face
  nearest in more than a quarter of its directions is inside a wall and weighs nothing --
  which is what lets a grid be placed from a building's bounds with no hand-tuned corner. `.cscn` authors them as `giVolumes: [{boxMin, boxMax, spacing,
  classify}]`. **Captures leave out what moves**: a skinned mesh, and any node with
  `capture_hidden`, its surface and its shadow both, since a capture taken while the game
  runs is kept for good.
- **IBL:** split-sum -- 32^3 irradiance map, GGX prefilter (1024 base, 9 mips), 512^2
  BRDF LUT. Environment comes from an HDR file (`-e/--env`) or is baked procedurally
  from the sky. Extracts bright light lobes from the HDR to aim analytic shadow lights.
  `reflect_fog` (spec 13.9, off by default) bakes the REFLECTION chains through the global
  height fog, lit by its ambient, the way Unreal's sky light captures its height fog; the
  irradiance is left alone, since that would re-light the scene. A probe's own capture is
  filtered through no medium.
- **Rain** (spec 13.9, `scene->rain`): falling streaks, splashes, wet surfaces with puddles
  and rings, the rain as a medium in the fog volume, and the cover it all respects. Since
  13.12: drops beaded and running on glass, drips off authored edges that go on after the rain
  stops, splashes that land on water rather than the bed under it, puddles that stand in a
  surface's own height map (`puddle_relief`, off by default), a mist lobe of its own, and the
  scene's `Wind` carrying the rain when it states an `air_speed`. See `rain.c/h` above and
  `docs/shader-subsystems.md`, Rain.
- **Sky:** Hillaire (EGSR 2020) physically-based atmosphere -- transmittance (256x64),
  multiple-scattering (32x32), and sky-view (192x108, re-baked when the sun moves) LUTs.
  Feeds the skybox, IBL, reflection probe, and fog; couples the directional key light
  via atmospheric transmittance.
  - **Scale:** RELATIVE by default, about four decades under a real sun, and
    `radiance_scale = SKY_PHOTOMETRIC_SCALE` puts it in nits beside photometric lights.
  - **What takes the scale:** everything the sun drives. The night floor, the stars and the moon
    are absolute, so a night frame is the same on either scale.
  - **Overcast:** `overcast` (0..1) lays a CIE overcast dome under cloud the sun cannot get
    through (spec 13.7).
- **Reflection probes:** local prefiltered cubemaps with parallax-corrected AABBs
  (Lagarde 2012); consumed as PBR specular and as the SSR ray-miss fallback. **Needs a
  precomputed IBL to be created at all**, so `--probe-scene` on a scene with no `-e` and
  no `--sky` logs "requires an HDR environment" and skips — and a `.cscn` authoring
  `probes[]` without an `environment` block is refused the same way, which is a trap
  worth knowing because the file still parses and the scene still renders.
  **Any number since spec 13.24, the nearest sixteen resident** (eight from 11.70 until
  then), authored as a top-level `probes: [{position, boxMin, boxMax, intensity, boxFade,
  envOnly}]` and blended per fragment; `--probe` beside an
  authored array is warned-ignored, and so is `environment.probe_scene`, because both
  ask for ONE auto-placed probe and a file with its own array has answered that already.
  Captured by the engine in the first frame, after the GI volume's opening sweep where
  there is one (spec 13.13), and published only once every probe succeeds, so no probe is
  ever photographed into another's capture. `--probe-set-res N` is the atlas row-0 size
  (default 512, any value in 64..2048 -- it briefly had to be a power of two, and
  the row table that removed that is the reason it no longer does), `--probe-set-probe N` the diagnostic, `--probe-set-debug` the atlas
  overlay — the only way to tell a bad column from a bad lookup, since both are the same
  picture from outside.
- **Derived area panels:** `--emissive-lights` (spec 11.49) fits a rectangle to each
  emissive mesh and registers it as a real `LIGHT_AREA` in the cluster list. Off by
  default. The fit is cached in LOCAL space and epoch-gated on the scene graph while
  PLACEMENT runs per frame from the owning node's transform, so a lamp on a moving node
  costs four vector transforms rather than a refit -- three corners through the node's 4x4, and
  the panel normal through its normal matrix. Reconcile, not rebuild: a panel whose
  mesh survives keeps the same `Light` object, so a `light_overrides` entry naming it
  persists across a graph change.

**Two captures, opposite requirements, and this is the thing to know before touching
either.** Both `gi_volume.c` and `probe.c` reach the scene through `scene_capture_faces`
and both raise `engine->capturing`, but a DDGI capture's output is IRRADIANCE that gets
added to the analytic direct term, so a derived emitter must be ABSENT from it or its
light arrives twice (measured: floor GI lift 1.31x). A reflection probe's output is
RADIANCE, what a mirror sees, so the emitter must be PRESENT. `engine->capture_kind`
names what a capture is FOR rather than that one is running, and only `gi_volume.c` asks for
IRRADIANCE — gating on `capturing` alone would settle the specular question by accident. That
specular double count is real, measured at 1.0386, and deliberately left in: every fix
costs more than it buys, and a gate arm bounds it instead.

**Both are lit DIFFUSE ONLY** (spec 13.13, `captureDiffuseOnly` in `pbr_frag`): no specular
lobe, direct or ambient, and a metal's base colour taken as its diffuse -- Unreal's rule for
its reflection captures. A reflection belongs to the eye that sees it, and while a capture is
taken a glossy surface has nothing to reflect but the global environment, a sky at infinity
that no roof blocks. A closed room's floor photographed reflecting the open day sky is what
gave every glossy surface in silent's house a grey wash. **And the environment's share of a
reflection is scaled by the GI volume's irradiance over the environment's** (`envVisible`,
capped at 1) -- the role Unreal gives its lightmap -- because the environment cannot know
about walls and the volume can. A probe's share is never scaled, having seen the room itself;
with no volume, and outdoors where the two agree, the scale is 1.

**No capture holds a window frozen** (spec 13.32). Silent's 792-probe home took 14 s of one frame
to sweep, and macOS beachballed. Three things changed, and only the last changes WHEN anything
lands:
- **The wall test draws depth only.** The classification's second capture runs the opaque lane
  through `SUBMIT_PASS_DEPTH_ONLY` with colour masked, and the shadow catcher through the one
  function the scene pass uses (`_render_capture_depth`). Every discard in `pbr_frag` is above
  the depth-only exit, so the depth is the shaded pass's, to the bit.
- **A capture gathers its lights once.** Every light whose reach meets the sphere the capture
  sees out to (`far * sqrt(3)`) is gathered once for its faces (`light_cluster_capture_begin`),
  the nearest kept past the cap, and each face assigns its cells from that list
  (`light_cluster_capture_face`). A face no wider than twice the 16x8 grid (a GI probe's 16 px)
  points every cell at the whole list, built once for its six faces: rebuilding the
  3,072-cell grid for each 16x16 face was more than half a GI capture, and the capture's index
  pool had been overflowing and dropping lights besides. A reflection probe's 2048 px faces
  keep their cells, which their fragments repay. The derived panels and the material variants
  are settled once a burst too, in `scene_capture_begin`, ahead of its shadow pass.
- **Every capture is paced by a time budget.** `Engine.capture_budget_ms` is the most of a
  frame the GI probes and the reflection probes' faces may take together. It is
  `ENGINE_CAPTURE_BUDGET_MS` (8) windowed and 0, no limit, headless, where the frame a capture
  lands in must not be the clock's to decide. Each KIND takes its first unit a frame, so
  neither starves the other and every capture finishes, and a unit is asked for only once it is
  in hand (`capture_budget_take`). The clock is read past a `glFinish`: a capture is CPU
  submission and GPU work, and without the sync the clock would see the submission alone. A
  reflection probe is taken a FACE a unit, every face of a frame in one burst, its cube held
  between frames, so its faces may see a flickering light differently. The opening sweep's
  one-frame exception (9.7, kept by 13.13 and 13.24) and `stream_rate` are gone.
  `stream-paced`, `stream-paced-walk` and `stream-paced-single` hold budgeted runs to the
  unbudgeted ones' lighting, to the bit: a still view, the teleport walk leaving a probe and a
  volume half captured, and a world of one probe.

**A capture sees the scene at rest, draws what its face can resolve, and is cooked** (spec
13.42). A GI capture's cost is per DRAW -- about 56 draws a face, each 20-40 µs on Apple's GL,
whose uniform changes copy the whole block -- and not triangles or targets, which decided what
was kept:
- **Two ways to draw less were built, measured and backed out.** Six faces in one layered draw:
  Apple's GL runs a geometry shader as a transform-feedback compute pass, slower than six draws.
  And only the chunks of a big mesh a face can see: 44% fewer triangles and a slower sweep, since
  every chunk is a draw.
- **Each item's level comes from the PROBE** (`draw_list_select_lod`, at the face's resolution,
  `draw_lod_bias_for`), not the camera, which was an input that moved with the player; the
  burst gives the camera's levels back as it ends. `GIWorld.cull_pixels` leaves out what spans
  fewer pixels than that across a face, keeping emitters and anything under a `capture_always`
  node; 0, everything, by default. It is the idea Lumen applies to its scene representation, settled per probe by the walk
  that picks the levels (`DrawItem.small`), so the shaded faces and the wall test leave out the
  same, and whether an item emits is asked there, in the burst, with what flickers held at rest.
- **A capture burst holds the scene at rest** (`scene_set_capture_rest`): `scene_capture_begin`
  calls the app's function with true before it settles the panels or draws its shadows,
  `scene_capture_end` with false, and the engine drives its own fires from a still flame
  (`fire_system_rest`) between them. An app holds what it animates and marks `capture_hidden`
  what moves every frame. So the frame a capture lands in no longer decides what it sees.
- **A GI volume's opening sweep and a reflection probe's column are COOKED** under one switch,
  `Scene.cook_lighting`, since a probe's key holds its volumes'. Off by default, since the suite
  shares one cook directory; `gi-cook` gives each arm its own. The key is `scene_capture_fold`
  (`capture_key.c`), folded in the burst, at rest, as the first probe is about to be drawn:
  - the engine's source as a build digest (`cetra/gen_build_digest.py`), and the driver's strings;
  - every setting a capture reads, through `config_snapshot_fold` -- the table walked keeping all
    but the rows named as not read, so a row added later is keyed by default;
  - every light that emits and reaches the box, whole, and every decal that meets it;
  - every item a capture of the box takes (`draw_item_captured_in`), by content, with its own
    material.

  A hit restores the texels (`lighting_atlas_cook_fetch`). A sweep whose scene changed before it finished logs
  `result=unstable` and stores nothing. Left out of the key: the cached shadow tiles' contents and
  the rain's cover, which follow how a sweep was paced, and a shadow cast into the reach from
  outside it.

## Modules

### `light.c/h`

Directional / point / spot / area lights. A point light carries a BODY, `source_radius` and `source_length` (a capsule centred on it, the length between its caps), which is where a cached shadow's softness comes from; a fire writes its flame's into its light each frame. A panel is refused one by name: its body is its rectangle, and a capsule would run along its normal. `shadow_follow` (spec 13.31) draws a cached light's kept faces again at any move rather than past `ShadowSystem.tile_tolerance`

### `shadow.c/h`

Cascaded shadow maps (CSM), PCF + optional PCSS; the per-frame punctual pool, sixteen layers, where an area light is a five-face cube in its own frame ending at its range (spec 13.27); and the cached point- and area-light TILES (specs 13.16 and 13.27): blocks of six faces a view kept by light identity, invalidated per face by a moved caster's box, carried across an array rebuild by blit, with a store cell of the still casters for each face that sees a mover, from a pool on top of the budget (spec 13.26). A swaying caster under no wind, or kept at rest by its material, is still to them. See Lighting Environment

### `probe_set.c/h`

N of them (spec 11.70), blended PER FRAGMENT: a per-froxel 16-bit mask on the light grid says which probes reach a cell, each proxy box gives a weight, and the leftover weight falls to the global environment through the same expression the no-probe path uses. Storage is octahedral roughness ROWS (mips would filter across the tile gutters) in the scene's lighting atlas on unit 14 — an atlas is a pool, so the second consumer cost a region and not a declaration. **Since 13.24 the set is the WORLD's** -- any number of probes, the nearest 16 RESIDENT (a column, a descriptor, a mask bit), each captured ONCE, its column read back and kept on the CPU and both of its cubes freed, so one that leaves residency comes back by upload. A probe is captured a face at a time under the frame's capture budget (spec 13.32), every face of a frame in one burst, nearest first, finishing one before starting another -- six large shaded faces and a prefilter are several hundred milliseconds -- and only while every GI volume its box touches is swept and every cached light reaching it holds a whole block (`gi_world_ready_in`, `shadow_tiles_cover`), since a capture is kept for good: one under way waits, keeping its faces, while that lapses, and one whose probe leaves residency is dropped with its cube. `scene_lighting_ready_in(box)` asks both halves, the volumes and the probes, for an app holding its view until the lighting round the eye is in. **The box fade runs OUTWARD from the proxy faces, and that is correctness rather than taste**: a floor lies ON the bottom face of the box that box-projects it, so an inward fade weighs every floor in every scene at exactly zero and hands the one surface the feature exists for back to the environment — which renders as a plausible frame, because a floor reflecting nothing and a floor reflecting a dim room are the same picture. **Per-DRAW selection is the refused alternative** and the fixture is built to refuse it: two rooms over ONE floor mesh, which is a single draw, so a per-draw design lights half of it with the wrong room's reflections while every per-room measurement still passes. **And an outward fade crosses any wall thinner than it** (spec 13.13): it is a FRACTION of the box's half-size, 0.2 by default, so silent's kitchen reached 0.37 m past its box, through a 0.1 m wall, and the great hall's panelling reflected a third kitchen. An app with rooms side by side sets each fade from its walls; the froxel mask is marked out to the box grown by its fade, not the bare box, or the reflection stops on a tile line. **The ENGINE captures a set, in the frame after the GI sweep, and waits for the volume's opening sweep** (spec 13.13, `probe_set_update`): a capture lights what it sees with the volume only once it has an answer, and with the environment's ambient before that, so a set captured at load photographed every closed room lit by the open sky, which by day every dark glossy surface reflected as a grey wash. So an app installs a set UNCAPTURED, each probe carrying its own planes and `environment_only`, and the set is inert until `ready` -- `probe_set_primary` answers NULL before then, which is what keeps one probe out of the others' captures; it did not, and an installed set's first probe was bound into every later capture through the single-probe path. The resident cap is 16, set by the mask (16 bits a froxel) and by VRAM (~4.3 MB per probe column); the world's count is not capped

### `lighting_atlas.c/h`

The ONE unit-14 texture every resident GI volume and reflection probe lives in (spec 13.24), owned by the SCENE: GI slots side by side, each as large as the largest volume in the world, then the probe columns, stacked where the GI slots are tall enough. It only GROWS, copying every slot across, so nothing resident is captured again for it; its capacity is min(K, world count), which is what kept the one-volume and two-probe goldens at their exact texture size. It replaced `probe_atlas.c/h`, the set's own atlas that the GI volume used to ADOPT -- once, refusing a second time, which is what ruled out two volumes

### `residency.c/h`

Which of a world's items hold the few resident slots, and each item's life in one (spec 13.24): the nearest to the camera by distance to their boxes, and a state -- out, capture, upload, loaded -- with the texels kept while it is out. No GL, no clock; the GI world and the probe set each hold one, so the two kinds share one life cycle rather than two copies that drift. Three rules: slots are STABLE (a holder keeps its slot while it stays resident), a holder is evicted only by an item more than a margin nearer, and an item coming back takes its HOME slot when free -- readmitted one column over, a kept probe's texels sample at UVs rounded differently, measured at 24 px on the round trip that is 0 px with homes. **Texels are kept at EVICTION**, read from the slot through the kind's callback before its next holder writes it, so a world that never evicts reads nothing back

### `light_cluster.c/h`

Clustered forward light culling: 16x8x24 frustum grid, std140 light UBOs. **Every bound in here may over-cover and must never under-cover**: a light, probe or decal left off a tile it reaches loses every pixel of that tile along a straight line on the grid. The screen-tile bound projects all EIGHT corners of a sphere's box (spec 13.13) -- the near four alone are the widest only on the side facing away from the screen centre, so a sphere wholly to one side of the view was cut short toward the middle. **The index pool is a 64 KB uniform block** (spec 13.51), over GL 4.1's guaranteed 16 KB, and the engine refuses a driver that allows less: at 16 KB silent's hall ran it dry, and every cluster after the one it ran out in shades with no local light, a hard line on a tile row that moves with the camera. The warning repeats while an overflow lasts, and `light-pool` is its arm

### `rain.c/h`, `rain_render.c/h`

Rain (spec 13.9), a Scene subsystem (`scene->rain`) owned like water. `rain.c` is the physics and the state and has no GL: Marshall-Palmer sizes, Atlas fall speeds, extinction and the flux of drops that splash, all from ONE knob, `rate_mmh`, with every other knob a scale defaulting to physical; and the wetness and puddle level, integrated in closed form per regime so the state does not depend on the frame rate (`settled` puts it at equilibrium at load). `rain_render.c` draws the drops and splashes from the post chain's late draw, binds the surface uniforms, and publishes the medium. The cover is `shadow.c`'s, a layer of the punctual array rendered along the rain (`shadow_render_rain_layer`), with a CPU query (`shadow_rain_cover_ask`/`_answer`) through a fixed-latency readback ring. **Three things since 13.12 outlive the rain itself.** `travel`, the direction it last fell, is what the cover map and the splashes read, so a pane under an eave stays dry the moment the rain stops rather than the map snapping to vertical. DRIP LINES (`rain_set_drip_lines`, up to `RAIN_DRIP_MAX` 48) keep dripping from the wetness after it stops, which is why the late draw asks `rain_draws` and not whether rain is falling; their slots are shared by rate times CYCLE, not rate times length -- a line's rate is already for its whole length, and weighting by length again handed a street's neighbour eaves most of the slots. And the rain follows a scene `Wind` that states an `air_speed` (m/s), along its gust cycle's MEAN for the cover and the splashes, which must not crawl, and its instantaneous gust for the drops. Everything a plausible frame hid while it was being built is in `docs/shader-subsystems.md`, Rain
