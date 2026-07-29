# Fix: 2D apps (`pcb`, `shapes`) skew the whole canvas on an empty-space drag

## Context

The `shapes` and `pcb` apps are meant to be **2D drag-and-drop** canvases. Instead, clicking
on empty background and dragging rotates the *entire* view in 3D — the flat squares shear into
parallelograms (see the user's screenshots). The user wants this fixed by giving these apps a
**proper orthographic 2D camera** (added to the engine, apps locked to it), and also wants
**`pcb` to get real drag-and-drop** (it currently has none).

## Root cause (confirmed from the code)

Both apps run a **perspective camera in `CAMERA_MODE_FREE`** (`shapes.c:220-241`,
`pcb.c:175-196`) and, every frame, unconditionally call `mouse_drag_update()`
(`shapes.c:143`, `pcb.c:102`). In FREE mode a normal (non-shift) left-drag runs the
**"orbit around look_at"** branch (`app.c:205-226`), gated *only* on `is_dragging`
(`app.c:182`) — it never checks whether a shape was actually picked. A left-press in empty
space still latches `is_dragging = true` (`engine.c:861`) even though ray-picking returns
`selected_node = NULL` (`engine.c:868`). Result: an empty-space drag orbits the perspective
camera around the origin → the flat squares skew.

(The shape-drag path *is* correctly gated on `selected_node` at `shapes.c:49`, so that part
works. `pcb`'s cursor callback is empty (`pcb.c:47`), so `pcb` only ever orbits.)

A global gate on `selected_node` would be wrong — the real 3D apps (`render`, `gametest`,
`tree`) *want* empty-space drag to orbit. The fix is therefore scoped to these two 2D apps
plus a new, opt-in orthographic camera in the engine.

## Approach

### 1. Engine — add an orthographic projection path (default off, perspective untouched)

**`cetra/src/camera.h` / `camera.c`**
- Add to `struct Camera`: `bool is_orthographic;` and `float ortho_height;` (world-space
  height of the view volume; width = `ortho_height * aspect_ratio`). Both construction paths
  must initialise them explicitly -- `create_camera` uses `malloc`, not `calloc`, and
  `import.c` hand-builds a second `Camera` -- or the byte that now selects the projection is
  indeterminate.
- New setter `set_camera_orthographic(Camera*, float ortho_height, float near_clip, float far_clip)`
  that sets `is_orthographic = true` and stores the params. Have `set_camera_perspective`
  set `is_orthographic = false` for symmetry.
- In `compute_projection_matrix` (`camera.c:259`): branch — if `is_orthographic`, build
  `glm_ortho(-w/2, w/2, -h/2, h/2, near, far)` with `h = ortho_height`, `w = h * aspect_ratio`;
  else the existing `glm_perspective`.

**`cetra/src/engine.c` — `update_engine_camera_perspective` (`engine.c:947`)**
- After setting `camera->aspect_ratio`, emit the projection via
  `compute_projection_matrix(camera, engine->projection_matrix)` instead of the hardcoded
  `glm_perspective`. For a perspective camera this is the identical `glm_perspective` call, so
  3D apps are byte-identical; for an ortho camera it now emits `glm_ortho`. Rendering picks
  this up automatically (`render.c:680` derives `view_proj`/`draw_projection` from
  `engine->projection_matrix`).

**Ortho-aware ray picking (so clicking/dragging a shape works under ortho)**
- `cetra/src/intersect.c/.h`: add `compute_ortho_ray_from_screen(sx, sy, fbw, fbh, projection,
  view, out_origin, out_dir)` — unproject the pixel's NDC near-point through
  `inv(projection*view)` for the ray origin, and set direction to the camera forward
  (parallel rays). (The existing `compute_ray_from_screen` assumes a single perspective eye
  point and is left unchanged.)
- In `_perform_engine_ray_picking` (`engine.c:2218`) and
  `get_mouse_world_position_on_drag_plane` (`engine.c:2197`): build the projection via
  `compute_projection_matrix(engine->camera, projection)`, then branch:
  - perspective → existing code path unchanged (origin = `camera->position`, dir from
    `compute_ray_from_screen`);
  - orthographic → origin/dir from `compute_ortho_ray_from_screen`, and use that origin (not
    `camera->position`) in `ray_point_at_distance`. Both functions use the same ray so the
    hit distance and drag-plane reconstruction stay consistent.

### 2. `shapes` app (`apps/shapes/src/shapes.c`)

- Replace the perspective setup (`shapes.c:232`) with `set_camera_orthographic(camera,
  ortho_height, near_clip, far_clip)`. Size `ortho_height` to preserve the current framing:
  `2 * distance * tan(fov/2)` with the current values (`distance = 300`, `fov = 0.37`)
  → **≈ 112**. Keep position `{0,2,300}`, look_at origin, up `+Y`, `near = 7`, `far = 10000`.
  Drop the now-meaningless `camera->theta`/`camera->height` orbit tuning (`shapes.c:237-238`).
- Remove the camera **orbit** wiring: delete the `mouse_drag_update` call in the render
  callback (`shapes.c:143-145`), and stop forwarding to / creating the `MouseDragController`
  (and its `camera_controller_on_key` in `key_callback`). The 2D camera no longer orbits.
- Keep the existing shape-drag (`shapes.c:48-68`) — it works unchanged once ortho picking
  lands.
- Add **pan-on-background-drag**: in `mouse_button_callback`, on left-press with
  `engine->input.selected_node == NULL`, capture pan start (cursor x/y + `camera->look_at` +
  `camera->position`); clear on release. In `cursor_position_callback`, add an
  `else if (is_dragging && panning)` branch that converts the cursor pixel delta to a world
  delta using the ortho extents (`ortho_width/fb_width`, `ortho_height/fb_height`) and
  translates both `look_at` and `position` by it (pure translation → no skew), then
  `update_engine_camera_lookat`. (Exact delta signs / coord space verified by running.)

### 3. `pcb` app (`apps/pcb/src/pcb.c`)

- Same camera swap to `set_camera_orthographic`, sized to *its* framing. `pcb` pushes content
  to `z = -700` via its model transform (`pcb.c:106`), so `distance = 1000` →
  `ortho_height = 2 * 1000 * tan(0.37/2)` ≈ **374**. Same orbit-wiring removal as `shapes`.
- **Add AABBs**: `pcb` never calls `calculate_aabb()`, so picking (which tests `mesh->aabb`)
  can't work. Add `calculate_aabb(meshN)` after each `generate_*_to_mesh` for the pickable
  shapes (`mesh1`–`mesh10`).
- **Implement shape drag-and-drop**: fill in the empty `cursor_position_callback`
  (`pcb.c:47`) by mirroring `shapes.c:48-68` (move `selected_node->original_transform[3][0/1]`
  on the drag plane). Add the same pan-on-background-drag as `shapes`. `pcb`'s model transform
  has no XY offset, so writing world XY into the node's local translation is correct.

## Files to modify

- `cetra/src/camera.h`, `cetra/src/camera.c` — ortho fields, `set_camera_orthographic`,
  `compute_projection_matrix` branch.
- `cetra/src/engine.c` — `update_engine_camera_perspective` via `compute_projection_matrix`;
  ortho branch in the two picking functions.
- `cetra/src/intersect.c`, `cetra/src/intersect.h` — `compute_ortho_ray_from_screen`.
- `apps/shapes/src/shapes.c` — ortho camera, remove orbit wiring, add pan.
- `apps/pcb/src/pcb.c` — ortho camera, remove orbit wiring, add AABBs, implement shape-drag + pan.

## Verification

1. **Build:** `./build.sh` completes clean (library + all apps).
2. **2D behavior (manual — these apps are windowed, not headless):**
   - Run `./out/bin/shapes`: dragging empty background **no longer skews** the view; dragging
     a square moves just that square; the squares stay perfectly rectangular at all times.
   - Run `./out/bin/pcb`: shapes are now **draggable**; empty-space drag pans without skew.
3. **3D regression (engine change must be a no-op for perspective):** capture a deterministic
   `render` frame before and after and confirm **0 differing pixels** — routing
   `update_engine_camera_perspective` through `compute_projection_matrix` calls the same
   `glm_perspective`, and the perspective picking path is untouched:
   ```
   ./out/bin/render -m my_models/raiden/source/raiden_textured_rigged.glb \
       -t my_models/raiden/textures -e my_models/studio_small_03_8k.hdr \
       -a my_models/animations/strut_walk.fbx -s my_models/animations/T-Pose.fbx \
       -x -f 120 --no-springs --no-auto-exposure -E 1.0 -S after.ppm
   magick compare -metric AE before.ppm after.ppm null:   # expect 0 (0)
   ```
   Also smoke-build/run `gametest` and `tree` to confirm no build breakage.

## As built

Landed as planned, with three additions the review forced.

**A latent out-of-bounds read had to be fixed first.** `traverse_and_pick` walked a mesh's
index buffer in threes on the assumption that every mesh is triangles. Line topology breaks
that: a bezier carries 38 indices, an unfilled circle 65, an unfilled sharp rect 5 -- none a
whole number of triangles, so the last iteration read one element past the array, and the
value read is used as a *vertex index*, turning a 4-byte overread into an unbounded one.
Giving pcb's shapes real AABBs is what made the loop reachable, so this shipped as part of
the same change: non-triangle meshes are now picked on their bounding box, which is also the
only meaningful answer for line geometry, and the triangle loop bound is `j + 2 <
index_count`. Confirmed with AddressSanitizer -- `heap-buffer-overflow` before, clean after.

**Both camera construction paths needed explicit initialisation** (`create_camera` and the
hand-built one in `import.c`), since a projection now branches on `is_orthographic`.

**The 2D cameras sit square-on to their content plane.** The old `y = 2` was leftover
perspective framing; with it, panning along world XY was not quite perpendicular to the view.

### Known limitation: ortho is honoured by projection and picking only

The rest of the engine still assumes a perspective frustum -- it reconstructs view space from
a hyperbolic depth (`shaders/include/depth.glsl`) and treats the eye as a single point. So
GTAO, SSR, clustered light culling, the PBR view vector, and (when enabled) DoF, froxel fog,
aerial perspective, TAA jitter, the skybox `.xyww` trick and CSM all mis-compute under an
orthographic camera. It does not show in these two flat apps, but an ortho camera is not yet
general engine support. `set_camera_orthographic` carries this caveat. Making it general
starts at `depth.glsl` -- the deliberate single choke point for that math -- plus the
depth-scaled wedge in `light_cluster.c`.
