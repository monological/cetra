# Lighting environment

Everything that lights a scene beyond its materials lives on the `Scene`: the lights, the shadow
system, the IBL, the sky, the GI volumes, the reflection probes and the rain. Each is installed by
a plain pointer write and owned by the scene from then on. GI volumes and reflection probes are
captured from the scene itself, and those captures share one set of rules, at the end of this
doc.

The shader side of several of these is in `docs/shader-subsystems.md`: Cascade soft shadows,
Area-light shadows, Cached shadows, Clustered specular probes, Rain, and the sky's sections.

## Lights

`light.c/h`: directional, point, spot and area (panel) lights. Intensity is photometric through
`units`; `range` is where the inverse-square falloff is windowed to zero, and the cull radius.

- **A point light may carry a body**: `source_radius` and `source_length`, a capsule centred on
  the light whose length is between its caps. It is where a cached shadow's softness comes from;
  a fire writes its flame's body into its light every frame.
- **A panel is refused a body**, by name: its body is its rectangle, and a capsule would run
  along its normal.
- **`shadow_follow`** draws a cached light's kept faces again at any move, rather than only past
  `ShadowSystem.tile_tolerance` (see Cached point- and area-light shadows).

## Clustered light culling

`light_cluster.c/h`: every local light (point, spot, area) reaches a fragment through a 16x8x24
froxel grid, with the lights in std140 uniform blocks. Probes and decals use the same grid.

- **Every bound may over-cover and must never under-cover.** A light, probe or decal left off a
  tile it reaches loses every pixel of that tile, along a straight line on the grid.
- **The screen-tile bound projects all eight corners of a sphere's box.** The near four alone are
  the widest only on the side facing away from the screen centre, so a sphere wholly to one side
  of the view is cut short toward the middle.
- **The index pool is a 64 KB uniform block** (spec 13.51), over GL 4.1's guaranteed 16 KB, and
  the engine refuses a driver that allows less. At 16 KB a dense room runs it dry, and every
  cluster after the one it ran out in shades with no local light: a hard line on a tile row that
  moves with the camera. The warning repeats while an overflow lasts; `light-pool` is its arm.

## Shadows

`shadow.c/h`, on `scene->shadow_system`. Three kinds of map share it: the directional cascades,
the per-frame punctual pool, and the cached tiles. The punctual pool, the rain's cover and the
cached tiles are all layers of one array.

### Cascades

Cascaded shadow maps: up to 3 cascades for up to 3 shadow-casting directional lights, in one
`GL_TEXTURE_2D_ARRAY`, each cascade fitted as a bounding sphere under an orthographic projection.
3x3 PCF by default, PCSS contact hardening optional.

**A cascade's soft-shadow thresholds are counted in texels of its map, never as a fraction of its
depth range** (spec 13.58). The range is mostly the scene pad the cascade's eye is pushed back by,
about 4% of the scene's radius, so a fraction of it drops every occluder shorter than that. See
`docs/shader-subsystems.md`, Cascade soft shadows.

### The per-frame punctual pool

Sixteen layers (`MAX_PUNCTUAL_SHADOW_LAYERS`), drawn every frame. A spot takes one layer, a point
light six, an area light five.

- **An area light is a five-face cube in the panel's own frame** (spec 13.27): its width axis,
  its up and its normal stand for x, y and z. The face behind the panel is never drawn, since the
  LTC answers zero there. A single 120-degree map down the normal is the rejected alternative: it
  reads every receiver more than 60 degrees off the normal as lit, so a panel lights the rooms
  round it through their walls. See `docs/shader-subsystems.md`, Area-light shadows.
- **A panel's faces end at its range**, not the scene's far plane. At the far plane a kitchen
  tube's side faces took in the whole street: 14.4 ms of GPU against 0.96 at the range.
- **A light takes all its layers or none.** Past a light the pool cannot fit, a later one needing
  fewer layers may still fit.
- **The cap is the driver's uniform limit**, not GL 4.1's guaranteed 1024 components: the uber
  program already uses 1029 at eight layers.

**The array is laid out for the frame's per-frame layers, and a capture keeps that layout** (spec
13.44). One layer is drawn at 4096, more at less as the budget allows, none at 1024, and the
cached tiles are laid out for that edge. The frame counts its layers before anything captures
(`shadow_punctual_frame_update`), and every pass of the frame lays the array out for at least
those. A capture holds the scene at rest, where a light may stop casting (silent puts its
flashlight away); laid out for the capture's fewer layers, the array would be rebuilt in the
capture and again in the frame, carrying every kept tile across both times. A light still casting
at rest is captured at the frame's edge, so the capture key folds that edge
(`shadow_punctual_edge_drawn`).

### Cached point- and area-light shadows

Specs 13.16 and 13.27. A point light or a panel with `cast_shadows`, `shadow_cache` and a range
keeps its faces as 256x256 tiles of the punctual array, past the per-frame layers and the rain's,
drawn once and kept, under a budget of their own (`PUNCTUAL_TILE_VRAM_BUDGET`, 192 MiB). That is
what lets a scene shadow dozens of candles where the per-frame pool stops at sixteen layers.

A light's tiles are a block of six faces per view, kept by light identity, invalidated per face
by a moved caster's box, and carried across an array rebuild by blit.

- **A panel is cached from its centre** over the same six world-axis faces a point light uses,
  with one view and no body: the per-frame cube's hard shadow at a tile's resolution.
- **The budget goes to the nearest lights.** Cached lights are ranked once a frame by the
  distance from the camera to their reach; a nearer light takes the block of the farthest holder
  more than 2 m farther. Once the tiles have opened, at most `tile_new_blocks_per_frame` (2)
  blocks are placed a frame, since a newly placed light draws every face at once.
- **A light without a block is unshadowed**, so it lights rooms through their walls. A GI or
  probe capture therefore waits until every cached light reaching its box holds a whole block
  (`shadow_tiles_cover`).

How the faces stay right, each a rule a plausible frame has broken:

- **A scene-graph change draws again only the faces it touches** (spec 13.38,
  `tiles_mark_graph_change`).
  - Draw-list items are matched to the faces' record by node serial and mesh id: never by place
    in the list, which one insertion shifts for every later item, and never by pointer, since a
    node freed and another created can share an address.
  - An item gone marks the box it was last drawn in, which the record keeps current as its node
    moves; an item new marks where it stands; an item held but uploaded again, or drawn
    otherwise, marks both.
  - Every face is drawn again only where that cannot answer: out of memory, a caster with no
    bound that is gone, new or changed, or the first frame after one with no note at all.
  - `tiles-graph-*` hold it; the render app's `--graph-churn` and `--graph-replace` reach it.
- **A light with a body is drawn from 8 views spread over it**, and its soft shadow is their
  average, each view's edge blurred under TAA to meet its neighbours'. A march through one cube
  map (Unreal's SMRT) is the rejected alternative: one view cannot tell a point it cannot see
  from a solid one, so the full shadow comes out the size of the centre's hard shadow.
  `tiles-truth` holds the result to a trace on `tile_core_fixture`.
- **A flame's flicker is a read, not a redraw**: the kept views are read where the body has moved
  them to, the centre's move times (dR - dB) / dB, so no face is drawn again.
- **A caster that moves is drawn over a copy.** A face that sees a mover keeps its still casters
  in a store cell and is that copy plus the movers each frame (Unreal's static/dynamic shadow
  caching). A mover still for 120 frames (`SHADOW_TILE_MOVER_HOLD`) becomes still again.
- **The store cells are a pool of 64 on top of the budget** (spec 13.26), one cell a face, given
  to faces drawn this frame, nearest light first. Inside the budget the lights would take every
  cell first and every face seeing a mover would be drawn whole each frame. A face keeps its cell
  while nothing moves in it, until a face on screen needs it. `--tile-stores 0` is the no-store
  path. A face left without a cell is warned about only when the camera's pass runs out: a
  capture's pass draws every face a mover reaches, in view or not, and would warn every frame a
  capture ran.
- **A swaying mesh is a mover only under a wind that moves it** (spec 13.26): a wind response
  under no wind, or under a wind of no strength, is still. And `cachedShadowWind: "rest"` on a
  material (Unreal's "Rigid") holds the caster at its rest pose in the kept faces, while the
  camera, the cascades and the per-frame lights keep the sway. Following a sub-millimetre sway on
  silent's 110 trees cost 176 ms of a 227 ms frame. A material change the graph cannot see
  redraws the faces of the caster it changed, not every face.
- **Glass casts nothing for a cached light**: the kept caster sets leave the blend and
  transmissive lanes out. Drawn solid, a clear pane shadows what is behind it.
- **A light with no body is read from exactly where its faces were drawn** (spec 13.31), so two
  things follow.
  - Moving, its shadow steps each time it passes `tile_tolerance`, unless it sets
    `shadow_follow`, which draws its faces again at any move (a swinging bulb, while its swing is
    wide).
  - Its lookup allows for float rounding as well as the map's quantization (`TILE_ROUNDINGS`). A
    face is drawn through a matrix carrying the light's world position, so its depths round at
    that size, while the lookup projects `worldPos - centre`, which does not. A surface squarely
    facing the light has one depth across the whole face, and without the allowance it passes or
    fails its own test all at once: under a moving light, a hard rectangle bounded by the face's
    edges comes and goes. The per-frame lookup reads through the matrix it drew with, its
    roundings largely cancel, and it needs no such allowance.

`--tile-reference M` draws every cached light from M views of its body as it is this frame,
redrawn each frame and averaged with no blur: the shadow by its definition, against which the
kept views are checked.

The lookup, the views' placement, the blur and the packing of a cached light's body into the
light block are in `docs/shader-subsystems.md`, Cached shadows.

## The sky

Hillaire's (EGSR 2020) physically based atmosphere, `sky.c/h`: a transmittance LUT (256x64), a
multiple-scattering LUT (32x32) and a sky-view LUT (192x108, re-baked when the sun moves). It
feeds the skybox, the IBL, the reflection probes and the fog, and couples the directional key
light through atmospheric transmittance.

- **Scale**: relative by default, about four decades under a real sun.
  `radiance_scale = SKY_PHOTOMETRIC_SCALE` puts it in nits beside photometric lights.
- **What takes the scale**: everything the sun drives. The night floor, the stars and the moon
  are absolute, so a night frame is the same on either scale.
- **Overcast**: `overcast` (0..1) lays a CIE overcast dome under cloud the sun cannot get through
  (spec 13.7).

The day/night cycle, the moon, the stars, the night floor, overcast and the scale are each a
section of `docs/shader-subsystems.md`.

## IBL

Split-sum image-based lighting, `ibl.c/h`: an irradiance cube (32x32 a face), a GGX prefilter
(1024 base, 9 mips) and a 512x512 BRDF LUT. The environment comes from an HDR file (`-e/--env`)
or is baked from the sky. From an HDR, the brightest lobes are extracted to aim analytic shadow
lights.

`reflect_fog` (spec 13.9, off by default) bakes the reflection chains through the global height
fog, lit by its ambient, the way Unreal's sky light captures its height fog. The irradiance is
left alone, since fogging it would re-light the scene. A probe's own capture is filtered through
no medium.

## GI volumes

DDGI (spec 9.7) over a world list (spec 13.24, `gi_volume.h`): any number of probe grids, one per
place, the nearest eight (`GI_RESIDENT_MAX`) resident in slots of the scene's lighting atlas.
`.cscn` authors them as `giVolumes: [{boxMin, boxMax, spacing, classify}]`.

- **What a fragment reads**: the volume it stands in, or just past one, the nearest swept
  volume's clamped edge, fading to the environment over three of that volume's cells (spec
  13.25). Inside a volume still sweeping it reads the environment. The fade exists because a
  grid's edge probes say nothing about ground far off, where every probe fails the visibility
  test: carried out across a hillside, the weights underflow to exactly zero and lay black lines
  along the box's planes.
- **Sweeps**: every sweep, the opening one at load included, takes probes nearest volume first
  under one capture burst while the frame's capture budget allows (see Captures), and a
  re-convergence no more than the world's `rate` besides.
- **Eviction**: a slot is read back and kept when it is evicted, so returning costs an upload. A
  volume evicted mid-sweep keeps nothing and sweeps again from a cleared slot, since a volume's
  tiles do not cover its whole rectangle and the last holder's texels would show between them.
- **Classification**: `create_gi_volume_spaced` lays a grid over a box with `classify` on. Each
  probe also captures its back faces, depth only, and a probe that finds a back face nearest in
  more than a quarter of its directions is inside a wall and weighs nothing. That is what lets a
  grid be placed from a building's bounds with no hand-tuned corner. See
  `docs/shader-subsystems.md`, Clustered specular probes, for the shader half.

## Reflection probes

Local prefiltered cubemaps with parallax-corrected boxes (Lagarde 2012), consumed as PBR specular
and as SSR's ray-miss fallback. `probe.c/h` is one probe; `probe_set.c/h` is the world's set,
blended per fragment (spec 11.70).

**Authoring.** A `.cscn` authors them as a top-level
`probes: [{position, boxMin, boxMax, intensity, boxFade, envOnly}]`. The world's count is not
capped; the nearest sixteen (`PROBE_SET_MAX`) are resident.

- **A probe needs a precomputed IBL to exist at all.** `--probe-scene` on a scene with no `-e`
  and no `--sky` logs "requires an HDR environment" and skips, and a `.cscn` authoring `probes[]`
  without an `environment` block is refused the same way. The file still parses and the scene
  still renders, which is what makes it a trap.
- **`--probe`** beside an authored array is warned and ignored, and so is
  `environment.probe_scene`: both ask for one auto-placed probe, and a file with its own array has
  answered that.
- **Diagnostics**: `--probe-set-res N` is the atlas's row-0 size (default 512, any value in
  64..2048), `--probe-set-probe N` the per-probe diagnostic, and `--probe-set-debug` the atlas
  overlay, the only way to tell a bad column from a bad lookup, since both are the same picture
  from outside. `RENDER_MODE_PROBE_COVERAGE` shows where a reflection falls through to the sky.

**The blend.** A per-froxel 16-bit mask on the light grid says which resident probes reach a
cell, each proxy box gives a weight, and the leftover weight falls to the global environment
through the same expression the no-probe path uses. Storage is octahedral roughness rows, not
mips, since mips would filter across the tile gutters, in the lighting atlas on unit 14. The
resident cap of 16 is set by the mask (16 bits a froxel) and by VRAM (about 4.3 MB a probe
column).

- **The box fade runs outward from the proxy faces, and that is correctness, not taste.** A floor
  lies on the bottom face of the box that box-projects it, so an inward fade weighs every floor
  at exactly zero and hands it back to the environment. That renders a plausible frame: a floor
  reflecting nothing and a floor reflecting a dim room look alike.
- **An outward fade crosses any wall thinner than it** (spec 13.13). It is a fraction of each
  axis's half-size, 0.2 by default, so a room's reflection can reach through a thin wall onto the
  next room. An app with rooms side by side sets each fade from its walls. The froxel mask is
  marked out to the box grown by its fade, not the bare box, or the reflection stops on a tile
  line.
- **Per-draw selection is the refused alternative.** The fixture is two rooms over one floor
  mesh, a single draw: a per-draw design lights half of it with the wrong room's reflections
  while every per-room measurement passes.

**Capture.** The engine captures a set, not the app (spec 13.13, `probe_set_update`).

- **An app installs a set uncaptured**, each probe carrying its own planes and `environment_only`.
  The set is inert until `ready`, and `probe_set_primary` answers NULL before then, which keeps
  one probe out of the others' captures through the single-probe path.
- **A set waits for the GI**: it is captured in the frame after the GI volume's opening sweep, or
  in the first frame where there is no volume. A capture lights what it sees with the volume only
  once the volume has an answer, and with the environment's ambient before that, so a set captured
  at load would photograph every closed room lit by the open sky.
- **Each probe is captured once**: a face at a time under the frame's capture budget, every face
  of a frame in one burst, nearest first, finishing one probe before starting another. Its column
  is read back and kept on the CPU and both of its cubes freed, so a probe that leaves residency
  comes back by upload.
- **A probe captures only while what it will keep is ready**: every GI volume its box touches is
  swept and every cached light reaching it holds a whole block (`gi_world_ready_in`,
  `shadow_tiles_cover`; `scene_capture_ready` states the whole rule). One under way waits,
  keeping its faces, while that lapses; one whose probe leaves residency is dropped with its cube.
- **The set is published only once every probe succeeds**, so no probe is photographed into
  another's capture.

`scene_lighting_ready_in(box)` asks both halves, the volumes and the probes, for an app that holds
its view until the lighting round the eye is in.

## The lighting atlas and residency

**`lighting_atlas.c/h`** is the one unit-14 texture every resident GI volume and reflection probe
lives in (spec 13.24), owned by the scene: GI slots side by side, each as large as the largest
volume in the world, then the probe columns, stacked where the GI slots are tall enough. It only
grows, copying every slot across, so nothing resident is captured again for it. Its capacity is
min(K, world count) for K resident slots.

**`residency.c/h`** decides which of a world's items hold the resident slots, and each item's life
in one: the nearest to the camera by distance to their boxes, in a state of out, capture, upload
or loaded, with its texels kept while it is out. It has no GL and no clock; the GI world and the
probe set each hold one, so the two kinds share one life cycle. Its rules:

- **Slots are stable**: a holder keeps its slot while it stays resident.
- **A holder is evicted only by an item more than a margin nearer.**
- **An item coming back takes its home slot when free.** Readmitted one column over, a kept
  probe's texels sample at UVs rounded differently.
- **Texels are kept at eviction**, read from the slot through the kind's callback before its next
  holder writes it, so a world that never evicts reads nothing back.

## Rain

Spec 13.9, `scene->rain`, a scene subsystem owned like water: falling streaks, splashes, wet
surfaces with puddles and rings, the rain as a medium in the fog volume, and the cover all of it
respects. Spec 13.12 added drops beaded and running on glass, drips off authored edges,
splashes that land on water, puddles standing in a surface's own height map (`puddle_relief`, off
by default), a mist lobe of its own, and wind.

- **`rain.c` is the physics and the state, and has no GL.** Marshall-Palmer drop sizes, Atlas
  fall speeds, the extinction and the flux of drops that splash all come from one knob,
  `rate_mmh`; every other knob is a scale defaulting to physical. The wetness and puddle level are
  integrated in closed form per regime, so the state does not depend on the frame rate;
  `settled` puts it at equilibrium at load.
- **`rain_render.c`** draws the drops and splashes from the post chain's late draw, binds the
  surface uniforms, and publishes the medium.
- **The cover is `shadow.c`'s**: a layer of the punctual array rendered along the rain
  (`shadow_render_rain_layer`), with a CPU query (`shadow_rain_cover_ask`/`_answer`) through a
  fixed-latency readback ring.

Three things outlive the rain itself:

- **`travel`**, the direction it last fell, is what the cover map and the splashes read, so a
  pane under an eave stays dry the moment the rain stops rather than the map snapping to vertical.
- **Drip lines** (`rain_set_drip_lines`, up to `RAIN_DRIP_MAX`, 48) keep dripping from the wetness
  after the rain stops, which is why the late draw asks `rain_draws` and not whether rain is
  falling. Their slots are shared by rate times cycle, not rate times length: a line's rate is
  already for its whole length.
- **Wind**: the rain follows a scene `Wind` that states an `air_speed` (m/s), along its gust
  cycle's mean for the cover and the splashes, which must not crawl, and its instantaneous gust
  for the drops.

The cover lookup, the streaks, splashes, wet surfaces, SSR on wet ground, glass and drips are in
`docs/shader-subsystems.md`, Rain.

## Derived area panels

`--emissive-lights` (spec 11.49, off by default) fits a rectangle to each emissive mesh and
registers it as a real `LIGHT_AREA` in the cluster list.

- **The fit is cached in local space** and epoch-gated on the scene graph, while placement runs
  per frame from the owning node's transform. A lamp on a moving node costs four vector transforms
  rather than a refit: three corners through the node's matrix and the normal through its normal
  matrix.
- **Reconcile, not rebuild**: a panel whose mesh survives keeps the same `Light` object, so a
  `light_overrides` entry naming it persists across a graph change.

## Captures

GI volumes and reflection probes both reach the scene through `scene_capture_faces`, and both
raise `engine->capturing`. Everything below applies to both unless it says otherwise.

### What a capture is for

The two have opposite requirements, and this is the thing to know before touching either.

- **A GI capture's output is irradiance**, added to the analytic direct term, so a derived
  emitter must be absent from it or its light arrives twice.
- **A reflection probe's output is radiance**, what a mirror sees, so the emitter must be present.

`engine->capture_kind` names what a capture is for, not merely that one is running; only
`gi_volume.c` asks for irradiance. Gating on `capturing` alone would settle the specular question
by accident. The specular double count that remains (an emitter both lit analytically and seen in
a probe) is deliberately left in, since every fix costs more than it buys; `emissive-spec` bounds
it.

### Lit diffuse only

Both captures are lit diffuse only (spec 13.13, `captureDiffuseOnly` in `pbr_frag`): no specular
lobe, direct or ambient, and a metal's base colour taken as its diffuse, which is Unreal's rule
for its reflection captures. A reflection belongs to the eye that sees it; while a capture is
taken a glossy surface has nothing to reflect but the global environment, a sky at infinity no
roof blocks, so a closed room's floor would be photographed reflecting the open sky.

**The environment's share of a reflection is scaled by the GI volume's irradiance over the
environment's** (`envVisible`, capped at 1), the role Unreal gives its lightmap, because the
environment cannot know about walls and the volume can. A probe's share is never scaled, having
seen the room itself. With no volume, and outdoors where the two agree, the scale is 1.

### What a capture sees

A capture is kept for good, so it must not depend on the frame it lands in.

- **Captures leave out what moves**: skinned meshes and any node with `capture_hidden`, both its
  surface and its shadow.
- **A capture burst holds the scene at rest** (`scene_set_capture_rest`, spec 13.42).
  `scene_capture_begin` calls the app's function with true before it settles the panels or draws
  its shadows, and `scene_capture_end` with false; between them the engine drives its own fires
  from a still flame (`fire_system_rest`). An app holds what it animates there and marks
  `capture_hidden` what moves every frame.
- **Each item's level of detail comes from the probe**, not the camera, which moves with the
  player: `draw_list_select_lod` at the face's resolution, through `draw_lod_bias_for`. The burst
  gives the camera's levels back as it ends.
- **`GIWorld.cull_pixels`** leaves out what spans fewer pixels than that across a face, keeping
  emitters and anything under a `capture_always` node; 0, the default, keeps everything. It is
  settled per probe by the walk that picks the levels (`DrawItem.small`), so the shaded faces and
  the wall test leave out the same items, and whether an item emits is asked there, in the burst,
  with what flickers held at rest.

### Pacing

**No capture holds a window frozen** (spec 13.32). Every capture is paced by a time budget:

- **`Engine.capture_budget_ms`** is the most of a frame the GI probes and the reflection probes'
  faces may take together: `ENGINE_CAPTURE_BUDGET_MS` (8) windowed, and 0, no limit, headless,
  where the frame a capture lands in must not be the clock's to decide.
- **Each kind takes its first unit a frame**, so neither starves the other and every capture
  finishes; a unit is asked for only once it is in hand (`capture_budget_take`).
- **The clock is read past a `glFinish`**: a capture is CPU submission and GPU work, and without
  the sync the clock would see the submission alone.
- **A reflection probe is taken a face a unit**, every face of a frame in one burst, its cube held
  between frames, so its faces may see a flickering light differently.

`stream-paced`, `stream-paced-walk` and `stream-paced-single` hold budgeted runs to the unbudgeted
ones' lighting, to the bit: a still view, a teleport walk leaving a probe and a volume half
captured, and a world of one probe.

### Cost

A GI capture's cost is per draw (about 56 draws a face, each 20-40 µs on Apple's GL, whose
uniform changes copy the whole block), not per triangle or per target.

- **The wall test draws depth only.** Classification's back-face capture runs the opaque lane
  through `SUBMIT_PASS_DEPTH_ONLY` with colour masked, and the shadow catcher through the one
  function the scene pass uses (`_render_capture_depth`). Every discard in `pbr_frag` is above the
  depth-only exit, so the depth is the shaded pass's, to the bit.
- **A capture gathers its lights once.** Every light whose reach meets the sphere the capture sees
  out to (`far * sqrt(3)`) is gathered once for its faces (`light_cluster_capture_begin`), the
  nearest kept past the cap, and each face assigns its cells from that list
  (`light_cluster_capture_face`). A face no wider than twice the 16x8 grid (a GI probe's 16 px)
  points every cell at the whole list, built once for its six faces; a reflection probe's 2048 px
  faces keep their cells, which their fragments repay.
- **The derived panels and the material variants are settled once a burst**, in
  `scene_capture_begin`, ahead of its shadow pass.
- **Two ways to draw less were measured and rejected.** Six faces in one layered draw: Apple's GL
  runs a geometry shader as a transform-feedback pass, slower than six draws. Drawing only the
  chunks of a big mesh a face can see: fewer triangles and a slower sweep, since every chunk is a
  draw.

### Cooking

A GI volume's opening sweep and a reflection probe's column are cooked (spec 13.42) under one
switch, `Scene.cook_lighting`, since a probe's key holds its volumes'. It is off by default,
because the suite shares one cook directory; `gi-cook` gives each arm its own. The cook itself is
`cook.c/h`, in `docs/architecture.md`.

The key is `scene_capture_fold` (`capture_key.c`), folded in the burst, at rest, as the first
probe is about to be drawn. It holds:

- the engine's source as a build digest (`cetra/gen_build_digest.py`), and the driver's strings;
- every setting a capture reads, through `config_snapshot_fold`: the snapshot table walked keeping
  all but the rows named as not read, so a row added later is keyed by default;
- every light that emits and reaches the box, whole, and every decal that meets it;
- every item a capture of the box takes (`draw_item_captured_in`), by content, with its material.

A hit restores the texels (`lighting_atlas_cook_fetch`). A sweep whose scene changed before it
finished logs `result=unstable` and stores nothing. Left out of the key: the cached shadow tiles'
contents and the rain's cover, which follow how a sweep was paced, and a shadow cast into the
reach from outside it.
