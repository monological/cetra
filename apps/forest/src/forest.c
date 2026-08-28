/*
 * forest -- a walkable kilometre of terrain, trees and rocks.
 *
 * Built to exercise spec 11.28's submission path with content rather than with
 * fixtures. Everything here is arranged so that instancing, LOD and frustum
 * culling all matter at once:
 *
 *   - a few PROTOTYPE meshes shared by thousands of nodes, so runs batch
 *   - prototypes grouped into contiguous sibling blocks, because the batcher
 *     joins only ADJACENT items and one foreign mesh between two instances ends
 *     the run
 *   - terrain split into PATCHES by a CDLOD quadtree, because a patch is the unit
 *     of both culling and LOD selection and a kilometre-wide mesh is neither.
 *     Before 11.63 that was a fixed tiles x tiles grid, which --no-quadtree still
 *     reaches -- the reason it stopped being one is that a fixed grid's count
 *     tracks the ground's AREA, so the same density over four kilometres is 1,024
 *     tiles where the quadtree draws 706
 *   - wind on the trees, which spec 11.53 made possible: a wind material used
 *     to be exempt from frustum culling entirely, and 2,000 uncullable trees
 *     defeat the point of the app
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// glew before GLFW, or GLFW pulls the system gl.h in first and glew refuses.
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "cetra/camera.h"
#include "cetra/engine.h"
#include "cetra/ibl.h"
#include "cetra/light.h"
#include "cetra/lod.h"
#include "cetra/cluster.h"
#include "cetra/material.h"
#include "cetra/mesh.h"
#include "cetra/noise.h"
#include "cetra/profiler.h"
#include "cetra/program.h"
#include "cetra/render.h"
#include "cetra/scene.h"
#include "cetra/shadow.h"
#include "cetra/sky.h"
#include "cetra/water.h"
#include "cetra/wind.h"
#include "cetra/transform.h"
#include "cetra/util.h"

#include "cetra/game/character.h"
#include "cetra/game/entity.h"
#include "cetra/game/game.h"
#include "cetra/game/input.h"
#include "cetra/game/physics.h"

#include "cetra/procedural/erosion.h"
#include "cetra/procedural/heightmap.h"
#include "cetra/procedural/rock.h"
#include "cetra/procedural/terrain.h"
#include "cetra/procedural/terrain_quadtree.h"
#include "cetra/procedural/terrain_stream.h"
#include "cetra/procedural/terrain_tex.h"
#include "cetra/procedural/tree_gen.h"
#include "cetra/procedural/vegetation_tex.h"
#include "cetra/texture.h"

// --- scene scale -----------------------------------------------------------

#define TREE_PROTOTYPES 6
#define ROCK_PROTOTYPES 8
// Props over the whole DOMAIN, which since 11.63 is not the same as props in the
// world: a region gets its share by area and the ones past the shore place
// nothing, so the island keeps about half. These are set so what stands on the
// land is the ~2000 trees and ~3000 rocks this app has always been about — a
// forest is the thing it exists to draw, and thinning it by half because the
// world grew a coastline would make every batching and LOD reading here a
// measurement of a sparser scene.
#define TREE_COUNT      3900
#define ROCK_COUNT      5900

// Collider resolution. Higher than it needs to be for Jolt and lower than the
// visual tiles, which is the trade: the character stands on this while the eye
// sees the tiles, and they diverge by about the height change across one
// collider quad.
#define COLLIDER_SEGMENTS 256

// The island (spec 11.63). Start is a fraction of the half-extent, so the shape
// is the same however big the world is; depth and sea level are world units,
// which they have to be -- they are measured against the fbm's own amplitude and
// not against the domain.
// Where the ground starts falling, as a fraction of the half-extent. The
// SHORELINE lands well outside it -- the falloff has only to carry the ground
// down to sea level, not to the sea floor -- so this is the number that decides
// how much of the domain is land, and 0.72 puts the waterline near 0.85.
#define ISLAND_START     0.72f
#define ISLAND_DEPTH     140.0f
#define ISLAND_SEA_LEVEL -35.0f
// How far above the waterline a prop's base has to be, in world units. Over the
// 1.21 the rock scatter can sink a boulder after sample_ground has answered, so
// the outermost props stand on the beach and not in the surf.
#define PROP_SHORE_MARGIN 2.5f

// Trees are generated at the generator's native scale (trunk_length ~125) and
// scaled down per instance rather than generated small. Leaf density is quoted
// per 10 units of arc, so a generator asked for a 12-unit trunk produces a bare
// canopy; scaling the node keeps the shape the parameters describe.
#define TREE_WORLD_SCALE 0.085f

typedef struct ForestArgs {
    int headless;
    int frames;
    const char* screenshot;
    int screenshot_every; // also save numbered frames every N (0 = only the final one)
    int profiler;
    int no_lod;
    // Bisect lever (spec 11.63): build lod.c chains instead of cluster DAGs.
    // NOT an off switch for LOD -- both fill the same lod_* ranges, so this
    // isolates which BUILDER produced them.
    int no_clusters;
    // Bisect lever (spec 11.63): the fixed tiles x tiles grid instead of the
    // CDLOD quadtree. Not an identity -- a quadtree draws a different surface at
    // a different density, which is the point of it -- so this exists to compare
    // against rather than to restore.
    int no_quadtree;
    // Bisect lever (spec 11.63): the CDLOD morph off in all five geometry
    // programs. The quadtree still selects and still draws the same patches --
    // only the blend toward the parent surface is gone, which is what makes this
    // the one lever that reaches the morph from a rendered frame.
    int no_morph;
    int quadtree_probe;
    // Domain half-width in world units; 0 keeps the app default. The only way to
    // ask what a bigger world costs, and what the quadtree is for.
    float terrain_extent;
    // Bisect lever (spec 11.63): one region over the whole domain, always
    // resident, which is what this app did before residency existed.
    int no_regions;
    int region_probe;
    float region_radius; // 0 keeps REGION_LOAD_RADIUS
    float region_span;   // 0 keeps REGION_SPAN_DEFAULT
    float walk;          // scripted character speed; 0 = keyboard only
    // The island is what this app IS since spec 11.63: ground that falls to a
    // shoreline and open sea past it. --no-island is the flat-domain terrain
    // every arm written before it measures, and it takes the sea with it.
    int no_island;
    // The gravel trail across the island (spec 11.68), which is also what keeps
    // props off its course. --no-trail is the ground before it.
    int no_trail;
    int no_instancing;
    int no_layers_vt;     // per-texel layered blend instead of the composite cache
    int layers_vt_res;    // composite-cache resolution override; 0 = derived
    int no_layers_vt_pages;    // fallback atlas alone -- stage 1 exactly
    int no_layers_vt_feedback; // residency on prediction alone (no vote pass)
    int layers_vt_page_slots;  // physical page slots in use; 0 = all
    int layers_vt_page_budget; // page bakes per frame; 0 = default
    int layers_vt_probe;       // print page residency every N frames; 0 = off
    int no_sort_opaque;   // opaque front-to-back ordering is on by default
    int depth_prepass;    // position-only depth before shading; off by default
    int force_taa;        // TAA headless too; diagnostic, costs determinism
    int msaa;             // requested sample count; 0 = the TAA/headless policy decides
    int headless_jitter;  // sub-pixel jitter headless; TAA is inert without it
    int render_mode;     // RenderMode override; 0 = PBR
    int no_spatial_sort; // scatter in draw order rather than Morton order
    int width, height;   // 0 = the default window size
    int no_sky;          // swap the atmosphere for a plain directional rig
    float sun_elevation; // degrees; < -900 keeps the app default
    float sun_azimuth;
    int no_aerial; // keep the sky, drop aerial perspective
    int no_fog;       // volumetric fog is on by default
    int trace_player; // log position, ground state and velocity each second
    float lod_bias;
    unsigned seed;
    int cam_set;
    vec3 cam_eye;
    vec3 cam_target;
    int water;         // flood the terrain to --water-level (spec 11.32)
    float water_level; // world Y of the still surface
    int erode;         // bake a heightfield and run erosion over it (spec 11.59)
    int erode_res;     // field resolution; 0 = EROSION_DEFAULT_RES
    int erode_iterations;
    int erode_workers; // 0 = size from the machine
    int erode_probe;   // print the bake's own numbers; implies --erode
    const char* erode_save;  // write the baked field here as .r16
    const char* heightmap;   // load a field from here instead of baking
    float heightmap_min;     // world Y the file's 0 maps to
    float heightmap_max;     // world Y the file's 65535 maps to
    // Stream a tiled field rather than holding one (spec 11.69). Opt-in by
    // naming a file, so the OFF leg is the whole-file load every terrain arm
    // already covers rather than a negative flag with nothing behind it.
    const char* terrain_stream;
    const char* terrain_stream_save; // write the installed field as a tiled file
    int terrain_stream_budget;       // tiles read per update; 0 = the default
    int terrain_stream_window;       // window edge in tiles; 0 = the default
    // Diagnostic: lower the node count at or under which a level stays whole,
    // so a fixture-scale field streams at all. Without it the whole of a 513
    // field is resident and there is nothing to measure.
    int terrain_stream_resident_res;
    int terrain_stream_probe; // print residency every N frames; 0 = off
    int height_probe;        // print sampled heights, normals and masks
    int scatter_probe;       // print where the scatter put things, against the drainage
    // Print each clustered prototype's DAG (spec 11.63). The instrument exists
    // because the guarantee is STRUCTURAL: "no cluster index leaves the original
    // vertex buffer" is what makes a crack impossible, and no frame can show it.
    int cluster_probe;
    // Place the whole world this far from the origin on X and Z (spec 11.62).
    // The instrument fp32's relative precision is measured with: at 0 this is
    // the frame the app has always rendered.
    float world_offset;
    // Diagnostic (--origin-shift-at): re-centre the world on the camera at this
    // frame, 0 = never. A shift is a TRANSITION, and a transition is invisible to
    // every headless arm until something can ask for one at a known frame -- the
    // same reason --shadows-off-at exists rather than being reachable by a state
    // flag. What it buys is a comparison: a shift changes only where coordinates
    // are measured FROM, so afterwards the frame differs from an unshifted run by
    // the precision it recovered and by nothing else.
    int origin_shift_at;
    // Camera drift the engine tolerates before re-centring on its own, 0 = never.
    // Reaches the same engine entry point the flag above does, on a threshold
    // instead of a frame number.
    float origin_shift_distance;
} ForestArgs;

static ForestArgs g_args;

static TerrainParams g_terrain;
// The CDLOD quadtree and the node it hangs its selection under. NULL under
// --no-quadtree, where the fixed tile grid fills that node once at startup
// instead.
static TerrainQuadtree* g_terrain_qt;
static SceneNode* g_terrain_group;
// The eroded field, when --erode is on. File-static because g_terrain borrows it
// for the process's lifetime -- the terrain is built once and never rebuilt.
static TerrainField g_field;
// The streamed field, when --terrain-stream names one (spec 11.69). It owns a
// TerrainField of its own, so exactly one of these two is ever installed.
static TerrainStream* g_stream;
static Scene* g_scene;
static SceneNode* g_root;
static Entity* g_player;
static ShaderProgram* g_pbr;

static Material* g_mat_terrain;
static Material* g_mat_bark;
static Material* g_mat_leaf;
static Material* g_mat_rock;

// Third-person orbit around the character. There is no follow-camera helper in
// cetra -- app.h offers only the mouse-drag orbit controller, which orbits a
// fixed point rather than a moving one.
static float g_cam_yaw = 0.6f;
static float g_cam_pitch = 0.28f;
static const float CAM_DISTANCE = 14.0f;

// Reported at startup, and the numbers the gate arms read from the log.
static size_t g_distinct_meshes;
static size_t g_prototype_tris;
static size_t g_node_count;
static size_t g_chains_built;
static size_t g_chains_refused;
static size_t g_clustered_meshes;
static size_t g_clusters_built;

// --- deterministic scatter -------------------------------------------------

// xorshift rather than rand(): the scatter must be identical across runs, and
// rand() is process-global state that the noise tables and any other seeder
// share.
static unsigned g_rng;

// Half-open [0, 1): the numerator is masked to 24 bits and the divisor is 2^24,
// so 1.0 is not reachable. Callers index prototype arrays with (int)(rnd() * N)
// and rely on that.
static float rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (float)(g_rng & 0xFFFFFFu) / (float)0x1000000u;
}

static float rnd_range(float lo, float hi) {
    return lo + (hi - lo) * rnd();
}

// --- mesh finalisation -----------------------------------------------------

/*
 * The one place a generated mesh becomes drawable. Either LOD builder REWRITES
 * mesh->indices, so the build has to precede the upload -- doing it after sends
 * level 0 and leaves every later level's offset pointing past the end of the
 * buffer.
 *
 * `cluster` picks which builder, and the split is measured rather than assumed
 * (spec 11.63).
 *
 * A cluster DAG locks each group's boundary at every level, which is what makes a
 * crack between levels impossible -- and on a REGULAR GRID that constraint costs
 * more than it buys, because whole-mesh simplification has no seams to respect
 * and reaches a better surface at the same triangle count. Terrain is exactly
 * that shape, and at matched picture cost the chain beat the DAG on it. Props are
 * not: a trunk or a rock is irregular, instanced thousands of times, and is where
 * 99% of this scene's triangles live.
 *
 * So props take the DAG and nothing else does. Note the ground reaches neither on
 * the shipping path: a quadtree patch is built at its own level and carries no
 * chain at all, so the terrain half of that comparison is live only under
 * --no-quadtree. Both builders fill the same lod_* ranges, so nothing downstream
 * -- selection, batching, the sort key -- learns which ran.
 */
static void finalize_mesh(Mesh* mesh, Material* material, bool cluster) {
    mesh->material = material;
    int levels = 0;
    MeshClusterStats st;
    if (cluster && !g_args.no_clusters && mesh_build_cluster_lod(mesh, &st)) {
        levels = mesh->lod_levels;
        g_clustered_meshes++;
        g_clusters_built += st.clusters;
        if (g_args.cluster_probe) {
            // foreign_indices is the SEAL: a coarse band referencing a vertex
            // band 0 never used did not come from the original buffer. max_index
            // rides along as context and asserts nothing on its own -- the
            // builder refuses an out-of-range input before it ever gets here.
            printf("cluster-probe mesh=%zu clusters=%d groups=%d dag_levels=%d bands=%d "
                   "max_index=%d vertex_count=%zu foreign=%d",
                   g_distinct_meshes, st.clusters, st.groups, st.levels, mesh->lod_levels,
                   st.max_index, mesh->vertex_count, st.foreign_indices);
            for (int b = 0; b < mesh->lod_levels; ++b)
                printf(" band%d=%zu", b, mesh->lod_count[b] / 3);
            printf("\n");
        }
    } else {
        levels = mesh_build_lod_chain(mesh);
    }
    if (levels > 1)
        g_chains_built++;
    else
        g_chains_refused++;
    upload_mesh_buffers_to_gpu(mesh);

    g_distinct_meshes++;
    g_prototype_tris += mesh->index_count / 3u;
}

static void set_node_trs(SceneNode* node, const vec3 pos, float yaw, const vec3 scale) {
    mat4 m;
    glm_mat4_identity(m);
    glm_translate(m, (float*)pos);
    glm_rotate_y(m, yaw, m);
    glm_scale(m, (float*)scale);
    glm_mat4_copy(m, node->original_transform);
}

static SceneNode* make_group(const char* name) {
    SceneNode* g = create_node();
    set_node_name(g, name);
    add_child_node(g_root, g);
    return g;
}

// --- scatter ---------------------------------------------------------------

// One scattered prop, before it becomes a node.
typedef struct Placement {
    vec3 pos;
    vec3 scale;
    float yaw;
    int proto;
    unsigned key; // Morton code of the terrain cell, for spatial ordering
} Placement;

static unsigned part1by1(unsigned n) {
    n &= 0x0000ffffu;
    n = (n | (n << 8)) & 0x00FF00FFu;
    n = (n | (n << 4)) & 0x0F0F0F0Fu;
    n = (n | (n << 2)) & 0x33333333u;
    n = (n | (n << 1)) & 0x55555555u;
    return n;
}

// Z-order rather than row-major. The batcher joins only CONSECUTIVE surviving
// items, so what matters is that props near each other in the world are near
// each other in the list -- a frustum then removes contiguous spans and leaves
// the survivors contiguous too. Row-major would hold only along one axis, and a
// camera looking across the rows would shred every run back to length one.
//
// It is the single largest thing standing between a scene like this and its
// batching; --no-spatial-sort exists so the difference can be measured rather
// than asserted, and specs/11.29 records the figures.
static unsigned morton_key(const TerrainParams* p, float x, float z) {
    float dx, dz;
    terrain_to_domain(p, x, z, &dx, &dz);
    float u = dx / (2.0f * p->extent);
    float v = dz / (2.0f * p->extent);
    u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    unsigned cx = (unsigned)(u * 1023.0f);
    unsigned cz = (unsigned)(v * 1023.0f);
    return part1by1(cx) | (part1by1(cz) << 1);
}

// Prototype first so each one is a contiguous block (a foreign mesh between two
// instances ends the run), then spatially within it.
//
// A pure function of its two arguments. --no-spatial-sort is honoured in the
// KEY (zeroed at fill time) rather than here, which produces the same comparator
// outputs over the same array and keeps the hidden global out of a comparator.
//
// Prototype grouping does not depend on this: region_emit filters by prototype,
// so each one is contiguous whatever order the array is in. Sorting on it anyway
// keeps the two agreeing about block order.
static int placement_cmp(const void* a, const void* b) {
    const Placement* pa = (const Placement*)a;
    const Placement* pb = (const Placement*)b;
    if (pa->proto != pb->proto)
        return pa->proto < pb->proto ? -1 : 1;
    if (pa->key != pb->key)
        return pa->key < pb->key ? -1 : 1;
    return 0;
}


// Its own table, not the global one: the texture bake reseeds the shared
// srand-backed generator, and a scatter that shifted depending on whether
// textures were baked first would not be reproducible.
static NoisePerm g_clump;

// Density field, in [0,1]. Without it the scatter is uniform, and a uniform
// scatter of one prototype reads as an orchard however good the tree is --
// clearings and thickets are what make it a forest.
static float clump_density(float x, float z, float freq) {
    float dx, dz;
    terrain_to_domain(&g_terrain, x, z, &dx, &dz);
    float u = dx * freq;
    float v = dz * freq;
    float n = noise_perlin3_tiled(&g_clump, u, 3.7f, v, 256);
    n = n * 0.5f + 0.5f;
    return n < 0.0f ? 0.0f : (n > 1.0f ? 1.0f : n);
}

// The drainage a tree tolerates standing in: the MIDPOINT of the band over which
// terrain_bake_splat turns the ground into a channel bed, i.e. where gravel
// stops being a tint and starts being what the surface is made of.
//
// Derived from the band rather than written down beside it. The first version
// was a literal 0.52 whose comment claimed it sat "below the gravel band's low
// edge... rejecting a few per cent of the map" -- and --scatter-probe measured
// it rejecting 43.9%, because flow's mean is near 0.49 and 0.52 is barely above
// it. A number that has to agree with another module's calibration should be
// computed from it.
#define TREE_MAX_FLOW ((TERRAIN_CHANNEL_FLOW_LO + TERRAIN_CHANNEL_FLOW_HI) * 0.5f)

/*
 * The trail (spec 11.68): a gravel path from the island's centre out to a shore,
 * as one road on the terrain material.
 *
 * Held in TERRAIN-LOCAL XZ. The material gets local + centre once, at bake, and
 * that authored frame is what the shader reads through authoredPos -- the same
 * frame splat_origin is built from, which forest deliberately never chases on an
 * origin shift. The scatter tests in local, so both sides move together and a
 * region rebuilt after a shift rejects the same ground.
 */
#define TRAIL_POINTS      12
#define TRAIL_WIDTH       3.0f
#define TRAIL_FEATHER     1.5f
#define TRAIL_LAYER       3 // gravel, the layer set's own worn surface
// The trail is handed to the material as ONE road, so it has to fit one road's
// point array -- nothing else connects the two, and overrunning it writes into
// point_count, width, feather and layer in turn.
_Static_assert(TRAIL_POINTS <= MATERIAL_MAX_ROAD_POINTS, "the trail must fit one road");
// How far past its own shoulder the trail keeps props off. A tree whose trunk
// clears the gravel but whose canopy hangs over it is still a tree in the path.
#define TRAIL_PROP_MARGIN 1.0f

static float g_trail_local[TRAIL_POINTS][2];
static int g_trail_points;
static unsigned long long g_trail_rejected;

// Its OWN stream, never rnd(): the scatter's draws are stream-aligned with every
// frame this app has ever rendered, and taking even one number from the shared
// generator here would re-place all five thousand props.
static unsigned trail_rand(unsigned* s) {
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static void build_trail(void) {
    g_trail_points = 0;
    g_trail_rejected = 0;
    if (g_args.no_trail)
        return;
    unsigned s = g_args.seed * 2246822519u + 3266489917u;
    float bearing = (float)(trail_rand(&s) % 6283) * 0.001f;
    float phase = (float)(trail_rand(&s) % 6283) * 0.001f;
    float dirx = sinf(bearing), dirz = cosf(bearing);
    float perpx = dirz, perpz = -dirx;
    float reach = 0.9f * g_terrain.extent;

    float prev_h = terrain_height_at(&g_terrain, terrain_world_x(&g_terrain, 0.0f),
                                     terrain_world_z(&g_terrain, 0.0f));
    for (int i = 0; i < TRAIL_POINTS; i++) {
        float t = (float)i / (float)(TRAIL_POINTS - 1);
        float r = t * reach;
        float wig = 0.04f * g_terrain.extent * sinf((float)i * 1.7f + phase);
        float bx = dirx * r + perpx * wig;
        float bz = dirz * r + perpz * wig;
        // Three candidates, and keep the flattest step. Not pathfinding: a path
        // that searches would wander somewhere the scatter cannot predict, and
        // what this needs is only to stop the trail climbing a cliff face. The
        // first point has no previous step to be flat against, so it takes the
        // bearing unnudged rather than entering the loop at all.
        float best_x = bx, best_z = bz, best_h = prev_h;
        if (i > 0) {
            float best_d = 1.0e9f;
            for (int k = -1; k <= 1; k++) {
                float nudge = 0.02f * g_terrain.extent * (float)k;
                float cx = bx + perpx * nudge, cz = bz + perpz * nudge;
                float h = terrain_height_at(&g_terrain, terrain_world_x(&g_terrain, cx),
                                            terrain_world_z(&g_terrain, cz));
                float d = fabsf(h - prev_h);
                if (d < best_d) {
                    best_d = d;
                    best_x = cx;
                    best_z = cz;
                    best_h = h;
                }
            }
        }
        // Stop at the shoal rather than running into the sea: past here the
        // ground the trail would cross is under water.
        if (i > 0 && g_terrain.island_start > 0.0f && g_terrain.island_start < 1.0f &&
            best_h < g_args.water_level + 2.0f * PROP_SHORE_MARGIN)
            break;
        g_trail_local[g_trail_points][0] = best_x;
        g_trail_local[g_trail_points][1] = best_z;
        g_trail_points++;
        prev_h = best_h;
    }
    if (g_trail_points < 2)
        g_trail_points = 0;
}

// Accumulate a region's trees for the scatter probe, which reads the whole
// distribution: a region is a sixteenth of it and says nothing on its own.
static Placement* g_probe_items;
static size_t g_probe_count, g_probe_cap;

static void probe_collect(const Placement* items, int count) {
    // The --scatter-probe guard lives INSIDE, not at the call sites. It was three
    // call-site ifs, which is three places to forget it and one silent wrong
    // answer when someone does -- these accumulate across every region load, so a
    // fourth caller without the guard makes the probe's distribution include
    // regions that were freed.
    if (!g_args.scatter_probe)
        return;
    if (!grow_array((void**)&g_probe_items, &g_probe_cap, g_probe_count + (size_t)count,
                    sizeof(Placement), 1024))
        return;
    memcpy(&g_probe_items[g_probe_count], items, (size_t)count * sizeof(Placement));
    g_probe_count += (size_t)count;
}

// The lowest prop of ANY kind, which is the whole of "nothing stands below the
// waterline". Rocks and not just trees, and that is the point: a tree is turned
// away from the shoal by the slope test long before the waterline matters, where
// a rock tolerates 57 degrees and the rim is 45, so the rock is the one the rule
// is actually for. Only maintained under --scatter-probe, which probe_low
// enforces itself rather than trusting its callers to.
static float g_lowest_prop_y = FLT_MAX;

static void probe_low(const Placement* items, int count) {
    if (!g_args.scatter_probe)
        return;
    for (int i = 0; i < count; ++i)
        if (items[i].pos[1] < g_lowest_prop_y)
            g_lowest_prop_y = items[i].pos[1];
}

/*
 * Where the scatter put things, against the drainage it was placing into.
 *
 * The instrument exists because the defect it guards is invisible in a frame: a
 * tree standing in a stream bed looks exactly like a tree, and the only thing
 * wrong with it is a number nothing prints. It reports the terrain's OWN flow
 * range beside the trees', which is what stops the arm being vacuous -- with no
 * erosion bake every mask reads zero, the placement rule is trivially satisfied,
 * and an arm checking only the trees would pass against a build that had never
 * implemented any of this.
 */
static void scatter_probe(const Placement* items, int count) {
    float tree_max = 0.0f, tree_sum = 0.0f;
    for (int i = 0; i < count; i++) {
        float f = terrain_mask_at(&g_terrain, TERRAIN_MASK_FLOW, items[i].pos[0], items[i].pos[2]);
        tree_sum += f;
        if (f > tree_max)
            tree_max = f;
    }
    printf("scatter-probe shore trees=%d low_y=%.6f water=%d water_level=%.6f\n", count,
           (double)g_lowest_prop_y, g_args.water ? 1 : 0, (double)g_args.water_level);

    // The terrain's own distribution, on a coarse grid over the same margin the
    // scatter draws from -- sampling the whole extent would include an edge the
    // scatter never reaches and report a range no tree could have hit.
    //
    // The FRACTION over the limit is the number that matters, not the peak.
    // erosion.c normalises flow to its own maximum, so a peak near 1.0 is true of
    // any sim that ran at all; what says candidates existed to reject is how much
    // of the domain the rule actually excludes.
    const int G = 64;
    float land_sum = 0.0f;
    int land_over = 0;
    float margin = g_terrain.extent * 0.96f;
    for (int j = 0; j < G; j++) {
        for (int i = 0; i < G; i++) {
            float x = terrain_world_x(&g_terrain, terrain_field_node(margin, G, i));
            float z = terrain_world_z(&g_terrain, terrain_field_node(margin, G, j));
            float f = terrain_mask_at(&g_terrain, TERRAIN_MASK_FLOW, x, z);
            land_sum += f;
            if (f > TREE_MAX_FLOW)
                land_over++;
        }
    }

    printf("scatter-probe trees=%d tree_flow_max=%.4f tree_flow_mean=%.4f "
           "land_flow_mean=%.4f land_frac_over=%.4f limit=%.4f\n",
           count, (double)tree_max, count ? (double)(tree_sum / (float)count) : 0.0,
           (double)(land_sum / (float)(G * G)), (double)land_over / (double)(G * G),
           (double)TREE_MAX_FLOW);

    // The trail's own row, APPENDED rather than folded into the line above: the
    // existing parsers are anchored on that line's token order.
    //
    // `on` is counted at the STRICT half-width, where the reject ran at
    // half-width plus feather plus margin. The asymmetry is what makes this a
    // real test of the CPU distance against the shader's: they are the same
    // formula written twice, and only a drift of more than the whole shoulder
    // could seat a tree on gravel the frame actually draws.
    int on = 0;
    for (size_t i = 0; i < g_probe_count; i++) {
        if (g_trail_points >= 2 &&
            roads_polyline_distance_xz(g_trail_local, g_trail_points,
                                       g_probe_items[i].pos[0] - g_terrain.center[0],
                                       g_probe_items[i].pos[2] - g_terrain.center[1]) <
                TRAIL_WIDTH * 0.5f)
            on++;
    }
    printf("scatter-probe road points=%d trees_on=%d rejected=%llu width=%.2f\n",
           g_trail_points, on, g_trail_rejected, (double)TRAIL_WIDTH);
}

// Rejection sampling against slope, against drainage, and against that density.
// Returns false when a candidate is unusable and the caller should draw another.
//
// `max_flow` is the drainage a prop tolerates standing in, and it is the half
// that was missing until spec 11.60. The ground already knew where its streams
// were -- terrain_bake_splat paints them gravel -- while the scatter placed by
// SLOPE alone, and a channel bed is flat, so it passed the slope gate with room
// to spare and the frame grew trees down the middle of a watercourse. Scoured
// bedrock had the same problem in reverse: the splat paints it rock precisely
// where the surface is shallow enough to plant on.
//
// 1.0 means "anywhere", which is right for a boulder: a rock in a stream bed is
// where rocks actually are.
static bool sample_ground(float max_slope, float max_flow, float clump_freq, float clump_power,
                          float x0, float z0, float span, vec3 out_pos) {
    float x = rnd_range(x0, x0 + span);
    float z = rnd_range(z0, z0 + span);
    // The domain's own margin still applies. A region's share is drawn against
    // its whole square and the part outside is REJECTED, so a region straddling
    // the margin concentrates its count inward rather than losing it, and one
    // entirely outside places nothing at all -- the attempt budget in the caller
    // is what stops that spinning. Both are what the margin is for: nothing
    // stands where terrain_normal_at is about to run off the field.
    //
    // Computed here rather than hoisted to the caller, deliberately. It is five
    // float ops against the four fbm evaluations below it, so hoisting buys
    // nothing measurable -- and every way of doing it is worse: a precomputed box
    // is a ninth parameter on a signature already carrying two, and a file static
    // is a value that goes stale the moment an origin shift moves the centre,
    // which regions_rebuild then re-scatters against.
    float margin = g_terrain.extent * 0.96f;
    float lo_x = terrain_world_x(&g_terrain, -margin), hi_x = terrain_world_x(&g_terrain, margin);
    float lo_z = terrain_world_z(&g_terrain, -margin), hi_z = terrain_world_z(&g_terrain, margin);
    if (x < lo_x || x > hi_x || z < lo_z || z > hi_z)
        return false;
    // Drainage before slope, because it is the cheaper of the two and rejects
    // the same candidates in either order: both are pure and neither draws from
    // rnd(), so the accepted set and the random stream are untouched by the
    // ordering. terrain_mask_at is one sample where terrain_normal_at is four
    // central-differenced height evaluations.
    //
    // Reads 0 with no erosion bake, so this degrades to exactly the pre-11.60
    // placement rather than to a special case -- the same property that lets
    // terrain_tint blend by the masks unconditionally. Which also means the
    // saving is real only under --erode; without a field this returns in two
    // instructions and never rejects.
    if (max_flow < 1.0f && terrain_mask_at(&g_terrain, TERRAIN_MASK_FLOW, x, z) > max_flow)
        return false;
    vec3 n = {0.0f, 1.0f, 0.0f};
    terrain_normal_at(&g_terrain, x, z, n);
    if (n[1] < max_slope)
        return false;
    // Nothing stands on the trail (spec 11.68), in the trail's own local frame
    // since that is the frame it is held in.
    //
    // LAST among the pure tests, which is the ordering rule stated above
    // applied rather than contradicted: the right order is ascending
    // cost-over-rejection-probability, and this corridor is ~0.2% of the domain
    // where the slope test rejects a large fraction of an island rim. Cheap per
    // call and almost never firing is what belongs at the end.
    //
    // Note this does NOT leave the random stream untouched -- x and z are drawn
    // at entry, so a rejection here still costs those two draws and the scatter
    // reports 1935 trees where it reported 1936. What makes the scatter
    // comparable run to run is trail_rand, not this position.
    if (g_trail_points >= 2 &&
        roads_polyline_distance_xz(g_trail_local, g_trail_points, x - g_terrain.center[0],
                                   z - g_terrain.center[1]) <
            TRAIL_WIDTH * 0.5f + TRAIL_FEATHER + TRAIL_PROP_MARGIN) {
        g_trail_rejected++;
        return false;
    }
    if (clump_freq > 0.0f) {
        float d = clump_density(x, z, clump_freq);
        if (rnd() > powf(d, clump_power))
            return false;
    }
    float y = terrain_height_at(&g_terrain, x, z);
    // Nothing STANDS below the waterline. The sea floor is terrain like any other
    // as far as the height function is concerned, and without this an island's
    // whole shoal is a drowned forest.
    //
    // The margin has to clear the deepest a caller sinks its prop AFTER this
    // returns: the rock scatter drops a boulder 0.22 of its scale into the
    // ground, which at the largest scale is 1.21 units. And the rock is what this
    // is really for -- a tree is turned away from the shoal by the slope test
    // long before the waterline matters, where a rock tolerates 57 degrees and
    // the rim is 45.
    //
    // Keyed on the ISLAND, not on whether a Water object exists. Those come apart:
    // the shaping is unconditional under --no-island, while water is refused when
    // the world is off the origin -- so --world-offset used to give the full drop
    // to the sea floor with no rejection at all, and props ran down the drowned
    // rim to y -175.
    if (g_terrain.island_start > 0.0f && g_terrain.island_start < 1.0f &&
        y < g_args.water_level + PROP_SHORE_MARGIN)
        return false;
    out_pos[0] = x;
    out_pos[1] = y;
    out_pos[2] = z;
    return true;
}

// --- scene construction ----------------------------------------------------

#define BARK_TEX_SIZE 1024
#define LEAF_CELL_SIZE 256
// The leaf cutout's alpha threshold, shared by the material that TESTS against
// it and the atlas bake that has to hold its coverage down the mip chain. The
// bake runs before the material exists, so neither can read it from the other,
// and a drift between them is silent -- the chain would be held to a threshold
// nothing tests.
#define LEAF_ALPHA_CUTOFF 0.4f

// One layer map is this square, and the number is NOT a memory decision even
// though it looks like one.
//
// Every tenant of the material texture array is resampled to one canonical size,
// which is the largest dimension of any source in the scene -- and in this app
// that is the leaf atlas, 8 cells of 256 laid out along u, so 2048. The array
// therefore costs 234.7 MB whatever this says, and a 512 map was being upsampled
// four times over in each axis to fill a layer it had no detail for. Measured
// both ways: 234.7 MB at 512 and at 1024, +1.7 s of bake in a DEBUG build (the
// preset build.sh defaults to, which passes no -O at all).
//
// So this buys detail that is already paid for. Raising it further is free in
// VRAM too, up to 2048, and stops being free the moment something else in the
// scene is larger than the leaf atlas.
#define TERRAIN_LAYER_TEX_SIZE 1024

// The splat is baked at the erosion field's own resolution when there is one, so
// it is an exact resample of the masks rather than an interpolation of an
// interpolation. With no field the masks read zero everywhere and this degrades
// to slope alone, which is the un-eroded look and is the point.
#define TERRAIN_SPLAT_FALLBACK_RES 512

// Ceiling on that when the field is STREAMED (spec 11.69). The splat is one
// texture over the whole domain, so following a streamed field's resolution
// would upload 201 MB at 8193 square to describe ground the eye never resolves
// -- and would read the whole file to bake it, which is the cost streaming
// exists to avoid. A bound rather than a fix: the splat's own pyramid is D10's.
#define TERRAIN_SPLAT_STREAM_MAX_RES 2048

// The four grounds, in splat order: layer 0 takes the remainder, then r, g, b.
// terrain_bake_splat decides which mask feeds which channel; this decides what
// each channel LOOKS like, which is the half an app gets to choose.
//
// One table rather than two parallel arrays and a ternary. `uv_scale` is world
// units per tile, and it belongs beside the kind for the same reason the kinds
// are ordered here: a coarse ground repeats over a longer distance than a fine
// one, which is a fact about that ground and not about the loop that bakes it.
static const struct {
    TerrainLayerKind kind;
    const char* name;
    float uv_scale;
} TERRAIN_LAYERS[] = {
    {TERRAIN_LAYER_GRASS, "grass", 4.0f},
    {TERRAIN_LAYER_ROCK, "rock", 6.0f},
    {TERRAIN_LAYER_SILT, "silt", 4.0f},
    {TERRAIN_LAYER_GRAVEL, "gravel", 3.0f},
};
_Static_assert(sizeof(TERRAIN_LAYERS) / sizeof(TERRAIN_LAYERS[0]) <= MATERIAL_MAX_LAYERS,
               "a fifth ground would arm a layer count the shader silently clamps away");

// The ground, as a layered surface (spec 11.60). Replaces a white albedo times a
// per-vertex tint at 2.6 units, which is why the terrain did not hold up at
// walking distance whatever the palette was.
static void bake_terrain_layers(Scene* scene) {
    const int T = TERRAIN_LAYER_TEX_SIZE;
    int count = (int)(sizeof(TERRAIN_LAYERS) / sizeof(TERRAIN_LAYERS[0]));
    for (int i = 0; i < count; i++) {
        unsigned char *albedo = NULL, *surface = NULL;
        terrain_layer_maps(TERRAIN_LAYERS[i].kind, T, g_args.seed + (unsigned)i * 977u, &albedo,
                           &surface);
        char key[64];
        snprintf(key, sizeof(key), "forest_layer_%s_a", TERRAIN_LAYERS[i].name);
        set_material_layer_albedo_tex(
            g_mat_terrain, i,
            texture_load_memory_owned(scene->tex_pool, key, albedo, T, T, 4, texture_desc(false)));
        snprintf(key, sizeof(key), "forest_layer_%s_s", TERRAIN_LAYERS[i].name);
        set_material_layer_surface_tex(
            g_mat_terrain, i,
            texture_load_memory_owned(scene->tex_pool, key, surface, T, T, 4, texture_desc(false)));
        g_mat_terrain->layers[i].uv_scale = TERRAIN_LAYERS[i].uv_scale;
    }

    int res = g_terrain.field ? g_terrain.field->res : TERRAIN_SPLAT_FALLBACK_RES;
    // One texture over the whole domain, so a streamed field's resolution is
    // the wrong thing to follow it with: 8193 square would be a 201 MB upload
    // of a map whose finest content the ground never resolves. Capped, and the
    // real answer is the splat pyramid D10 owns -- this is a bound, not a fix.
    if (g_terrain.field && g_terrain.field->stream && res > TERRAIN_SPLAT_STREAM_MAX_RES)
        res = TERRAIN_SPLAT_STREAM_MAX_RES;
    unsigned char* splat = malloc((size_t)res * (size_t)res * 3u);
    if (splat && terrain_bake_splat(&g_terrain, res, splat)) {
        set_material_splat_tex(g_mat_terrain,
                               texture_load_memory_owned(scene->tex_pool, "forest_terrain_splat",
                                                         splat, res, res, 3, texture_desc(false)));
    } else {
        free(splat);
        fprintf(stderr, "forest: splat bake failed; the ground falls back to layer 0\n");
    }

    // The splat is a function of world XZ over the terrain's own square, which
    // is what makes it work at all here: terrain tiles carry no UV1 (build_grid
    // writes a literal zero), so a mesh-local reading samples one texel and the
    // whole kilometre resolves to layer 0.
    g_mat_terrain->splat_space = SPLAT_SPACE_WORLD_XZ;
    g_mat_terrain->splat_origin[0] = terrain_world_x(&g_terrain, -g_terrain.extent);
    g_mat_terrain->splat_origin[1] = terrain_world_z(&g_terrain, -g_terrain.extent);
    g_mat_terrain->splat_size[0] = g_mat_terrain->splat_size[1] = 2.0f * g_terrain.extent;

    // The trail, in the authored frame the splat rectangle above is in: the
    // shader reads a road through authoredPos, so local + centre once here is
    // what keeps the two agreeing after an origin shift.
    build_trail();
    if (g_trail_points >= 2) {
        MaterialRoad* road = &g_mat_terrain->roads[0];
        memset(road, 0, sizeof(*road));
        for (int i = 0; i < g_trail_points; i++) {
            road->points[i][0] = terrain_world_x(&g_terrain, g_trail_local[i][0]);
            road->points[i][1] = terrain_world_z(&g_terrain, g_trail_local[i][1]);
        }
        road->point_count = g_trail_points;
        road->width = TRAIL_WIDTH;
        road->feather = TRAIL_FEATHER;
        road->layer = TRAIL_LAYER;
        g_mat_terrain->road_count = 1;
        printf("Trail: %d points, %.1f wide in layer %d\n", g_trail_points, (double)TRAIL_WIDTH,
               TRAIL_LAYER);
    }

    // Last, because it is what arms the shader.
    g_mat_terrain->layer_count = count;
    // And the mesh side of the same switch, which must be set before
    // build_terrain writes a single vertex colour: the layers carry the ground's
    // colour now, so the tint has to become macro variation or the two multiply
    // and the ground comes out near black.
    g_terrain.layered = true;
    printf("Terrain layers: %d at %dx%d, splat %dx%d\n", count, T, T, res, res);
}

// Bark and foliage, synthesised rather than loaded -- the app ships no assets.
// Without the leaf atlas's alpha channel the cards render as solid quads, which
// is the difference between a canopy and a cloud of confetti.
static void bake_vegetation_textures(Scene* scene) {
    const int B = BARK_TEX_SIZE;
    float* field = malloc((size_t)B * B * sizeof(float));
    if (field) {
        veg_bark_height_field(field, B, B);
        set_material_albedo_tex(g_mat_bark,
                                texture_load_memory_owned(scene->tex_pool, "forest_bark_albedo",
                                                          veg_bark_albedo(B, B, field), B, B, 3,
                                                          texture_desc(true)));
        // Stated as a NORMAL so it takes BC5. Procedural maps reach the GPU
        // through this path rather than through an importer, so nothing else can
        // know what they are.
        set_material_normal_tex(
            g_mat_bark,
            texture_load_memory_owned(scene->tex_pool, "forest_bark_normal",
                                      veg_bark_normal(B, B, field), B, B, 3,
                                      (TextureDesc){.is_srgb = false,
                                                    .alpha = TEXTURE_ALPHA_DATA,
                                                    .use = TEXTURE_USE_NORMAL}));
        set_material_roughness_tex(g_mat_bark,
                                   texture_load_memory_owned(scene->tex_pool, "forest_bark_rough",
                                                             veg_bark_roughness(B, B, field), B, B,
                                                             3, texture_desc(false)));
        free(field);
    }

    const int LW = LEAF_CELL_SIZE * TG_LEAF_VARIANTS;
    const int LH = LEAF_CELL_SIZE;
    unsigned char *la = NULL, *ln = NULL, *lr = NULL;
    veg_leaf_cluster_maps(LW, LH, &la, &ln, &lr);
    // The alpha-TESTED image, so the one whose chain has to hold its coverage
    // against the cutoff the material below states.
    TextureDesc leaf_desc = texture_desc(true);
    leaf_desc.coverage_cutoff = LEAF_ALPHA_CUTOFF;
    set_material_albedo_tex(g_mat_leaf,
                            texture_load_memory_owned(scene->tex_pool, "forest_leaf_albedo", la, LW,
                                                      LH, 4, leaf_desc));
    set_material_normal_tex(g_mat_leaf,
                            texture_load_memory_owned(scene->tex_pool, "forest_leaf_normal", ln, LW,
                                                      LH, 3,
                                                      (TextureDesc){.is_srgb = false,
                                                                    .alpha = TEXTURE_ALPHA_DATA,
                                                                    .use = TEXTURE_USE_NORMAL}));
    set_material_roughness_tex(g_mat_leaf,
                               texture_load_memory_owned(scene->tex_pool, "forest_leaf_rough", lr,
                                                         LW, LH, 3, texture_desc(false)));
}

static Material* make_material(const char* name, vec3 albedo, float roughness, float metallic) {
    Material* m = create_material();
    m->name = safe_strdup(name);
    glm_vec3_copy(albedo, m->albedo);
    m->roughness = roughness;
    m->metallic = metallic;
    set_material_shader_program(m, g_pbr);
    add_material_to_scene(g_scene, m);
    return m;
}

// WaterHeightFn over the terrain. A thin adapter rather than a cast, because
// terrain_height_at takes its params first and the water system passes a context
// pointer -- and because the signature is the contract, so it should be written
// out where a reader can see the two agree.
static float forest_bed_height(void* ctx, float x, float z) {
    return terrain_height_at((const TerrainParams*)ctx, x, z);
}

// 1.96 units between cells at the default sizing, against the visual mesh's 2.60,
// so the field resolves everything the tiles can draw with a little to spare.
// Raising it is a straight quadratic on the bake: 452 ms on eight threads, 1839
// on one. Those were taken at 512 rather than at this 513, so read them as the
// size and not as this constant -- one extra row and column is 0.4% of the cells
// and well inside the run-to-run spread, but the numbers are not measurements of
// what the app now bakes.
//
// RELEASE figures. build.sh defaults to the debug preset, which passes no -O at
// all, and the same bake there is 2.3 s and 14.8 s -- five times slower, because
// none of the sim's small helpers inline. Any timing taken from `out/bin` rather
// than `out/release/bin` is measuring that.
//
// 513 and not the power of two it looks like it should be. The grid is
// NODE-CENTRED -- texel 0 on -extent, res-1 on +extent -- so it has res-1 cells,
// and a mip pyramid halves only while that is even (spec 11.63). At 512 the
// field gets no levels at all and every coarse quadtree patch point-samples the
// full-resolution surface; at 513 it gets eight -- 513, 257, 129, 65, 33, 17, 9
// and 5, where terrain_field_build_pyramid stops because a 4-tap kernel over
// fewer than five nodes is measuring its own clamp.
#define EROSION_DEFAULT_RES 513

// Seed a field from the fbm, erode it, and install it. Everything downstream --
// tiles, collider, scatter, water bed, camera -- then reads the eroded surface
// through the same terrain_height_at they already called, which is the whole
// point of the field being a source rather than a subsystem.
static void bake_erosion(void) {
    int res = g_args.erode_res > 0 ? g_args.erode_res : EROSION_DEFAULT_RES;
    if (!terrain_field_alloc(&g_field, res)) {
        fprintf(stderr, "forest: erosion field %dx%d allocation failed\n", res, res);
        return;
    }
    if (!terrain_field_seed(&g_field, &g_terrain)) {
        terrain_field_free(&g_field);
        fprintf(stderr, "forest: erosion field seed refused\n");
        return;
    }

    ErosionParams ep = erosion_default_params();
    if (g_args.erode_iterations > 0)
        ep.iterations = g_args.erode_iterations;
    ep.workers = g_args.erode_workers;

    // Zero iterations installs the SEEDED field and runs no sim. Not a degenerate
    // case to tolerate but the one configuration in which the field and the fbm
    // must agree exactly, which is what makes it worth a flag value: sampling a
    // field at a node has to return the fbm value that was written there, and
    // that is the whole node convention asserted end to end.
    if (g_args.erode_iterations < 0) {
        terrain_field_measure(&g_field);
        g_terrain.field = &g_field;
        printf("Terrain seeded: %dx%d cells at %.2f units, no erosion\n", res, res,
               (double)terrain_field_cell(g_terrain.extent, res));
        return;
    }

    ErosionStats st;
    double t0 = glfwGetTime();
    bool ok = terrain_erode(&g_field, &g_terrain, &ep, &st);
    double ms = (glfwGetTime() - t0) * 1000.0;
    if (!ok) {
        terrain_field_free(&g_field);
        fprintf(stderr, "forest: erosion bake refused\n");
        return;
    }

    // Installed only now. Seeding through a params that already pointed at the
    // field would have sampled the plane it was filling.
    g_terrain.field = &g_field;
    float cell = terrain_field_cell(g_terrain.extent, res);
    printf("Terrain eroded: %dx%d cells at %.2f units, %d iterations, %d threads, %.0f ms\n", res,
           res, cell, ep.iterations, st.workers, ms);

    if (g_args.erode_save) {
        float lo = g_field.min_y, hi = g_field.max_y;
        if (heightmap_save(&g_field, g_args.erode_save, lo, hi) && g_args.erode_probe) {
            // At full precision, and this row is why: the range is not recorded in
            // a headerless file, so reading the terrain back needs a number the
            // log's three decimals cannot carry.
            printf("terrain-erosion-probe saved path=%s min=%.9g max=%.9g\n", g_args.erode_save,
                   (double)lo, (double)hi);
        }
    }

    if (g_args.erode_probe)
        erosion_stats_probe(&st, &ep, res, cell, ms);
}

// Install a heightfield from a file, in place of baking one. This is the AAA path
// and the reason the bake writes what this reads: a Gaea or World Machine export
// arrives through exactly here.
static void load_heightfield(void) {
    float lo = g_args.heightmap_min, hi = g_args.heightmap_max;
    if (!(hi > lo)) {
        // The fbm this replaces produces roughly [-height, +height], so a file
        // with no stated range is read as covering the same span.
        lo = -g_terrain.height;
        hi = g_terrain.height;
    }
    if (!heightmap_load(&g_field, g_args.heightmap, lo, hi)) {
        fprintf(stderr, "forest: heightmap %s refused; keeping the analytic terrain\n",
                g_args.heightmap);
        return;
    }
    g_terrain.field = &g_field;
    printf("Terrain loaded: %s, %dx%d, range %.3f..%.3f\n", g_args.heightmap, g_field.res,
           g_field.res, (double)lo, (double)hi);
}

// Install a streamed field (spec 11.69). `l0_coverage` comes from the caller
// because it is a REGION number and the region constants are declared further
// down: the radius level 0 must serve without falling is the one region loads
// use, not the camera's, since a region load builds a collider and a scatter --
// both of which want exact heights -- out to its own load distance whatever the
// eye is looking at.
static void load_stream(float l0_coverage) {
    double t0 = glfwGetTime();
    g_stream = terrain_stream_open(g_args.terrain_stream, g_terrain.extent, l0_coverage,
                                   g_args.terrain_stream_resident_res,
                                   g_args.terrain_stream_window, g_args.terrain_stream_budget);
    double ms = (glfwGetTime() - t0) * 1000.0;
    if (!g_stream) {
        fprintf(stderr, "forest: terrain stream %s refused; keeping the analytic terrain\n",
                g_args.terrain_stream);
        return;
    }
    g_terrain.field = &g_stream->field;
    printf("terrain-stream-header path=%s res=%d levels=%d tile=%d min=%.9g max=%.9g ms=%.0f\n",
           g_args.terrain_stream, g_stream->field.res, g_stream->level_count, g_stream->tile_nodes,
           (double)g_stream->field.min_y, (double)g_stream->field.max_y, ms);
}

static void save_stream(void) {
    if (!terrain_stream_save(&g_field, g_args.terrain_stream_save)) {
        fprintf(stderr, "forest: could not write the terrain stream %s\n",
                g_args.terrain_stream_save);
        return;
    }
    printf("terrain-stream-probe saved path=%s res=%d levels=%d tile=%d min=%.9g max=%.9g\n",
           g_args.terrain_stream_save, g_field.res, g_field.level_count,
           TERRAIN_STREAM_TILE_NODES, (double)g_field.min_y, (double)g_field.max_y);
}

// Patch edge the quadtree aims its finest level at, in world units.
//
// The LEVEL COUNT is derived from it rather than fixed, so growing the domain
// adds a level instead of coarsening the ground underfoot -- which is the whole
// property a quadtree is here for and the one a tiles x tiles grid cannot have.
// At TERRAIN_PATCH_SEGMENTS quads that makes the finest cell about two units,
// a little tighter than the 2.6 the fixed grid used.
#define TERRAIN_PATCH_SPAN     32.0f
#define TERRAIN_PATCH_SEGMENTS 16

static int forest_quadtree_levels(float extent) {
    int levels = 1;
    while (levels < 16 && (2.0f * extent) / (float)(1 << (levels - 1)) > TERRAIN_PATCH_SPAN)
        levels++;
    return levels;
}

// Whether this run ever moves the world off the storage origin, by any of the
// three routes into it. One place, because the answer gates water in two and the
// island's own sea-level default in a third, and the routes are easy to enumerate
// incompletely -- --origin-shift-at was missing from both water tests for a spec
// cycle, so a scheduled shift created a surface and then moved the world out from
// under its origin-anchored bed.
static bool world_leaves_origin(void) {
    return g_args.world_offset != 0.0f || g_args.origin_shift_distance > 0.0f ||
           g_args.origin_shift_at > 0;
}

static void build_terrain(void) {
    SceneNode* group = make_group("terrain");
    if (g_args.no_quadtree) {
        for (int tz = 0; tz < g_terrain.tiles; ++tz) {
            for (int tx = 0; tx < g_terrain.tiles; ++tx) {
                Mesh* mesh = create_mesh();
                if (!terrain_build_tile(&g_terrain, tx, tz, mesh)) {
                    free_mesh(mesh);
                    continue;
                }
                finalize_mesh(mesh, g_mat_terrain, false);

                SceneNode* node = create_node();
                add_mesh_to_node(node, mesh);
                add_child_node(group, node);
                g_node_count++;
            }
        }
    } else {
        // Nothing is built here. The first descent -- which needs a camera, and
        // so cannot happen before the first frame -- builds what it selects.
        g_terrain_group = group;
        g_terrain_qt = create_terrain_quadtree(&g_terrain, group,
                                               forest_quadtree_levels(g_terrain.extent),
                                               TERRAIN_PATCH_SEGMENTS, g_mat_terrain);
        if (!g_terrain_qt)
            fprintf(stderr, "forest: terrain quadtree refused\n");
    }

    // Collision belongs to the regions -- see region_load. There is deliberately
    // no whole-domain body here: one would be an exact duplicate of the region
    // colliders (same lattice, same height function, same cell), so the physics
    // world would carry every ground triangle twice and a character would stand
    // on the global one. That made region-collider assert nothing, since deleting
    // every per-region collider left the arm green.
}

// The prototypes, built once and shared by every instance in every region. Held
// as file statics rather than returned, because a region that loads three
// seconds into the run has to find them and rebuilding a tree per region would
// cost more than the instances do.
static Mesh* g_bark[TREE_PROTOTYPES];
static Mesh* g_leaf[TREE_PROTOTYPES];
static Mesh* g_rocks[ROCK_PROTOTYPES];

static void build_tree_prototypes(void) {

    for (int i = 0; i < TREE_PROTOTYPES; ++i) {
        TreeParams tp;
        memset(&tp, 0, sizeof(tp));
        tp.seed = 101 + i * 37;
        // Depth 3 rather than the tree app's 4: a hero tree is 107k triangles,
        // and two thousand of those would be 200M submitted before any culling.
        tp.max_depth = 3;
        tp.trunk_length = 125.0f;
        tp.trunk_radius = 8.0f + (float)(i % 3);
        tp.branches_per_node = 3;
        tp.length_decay = 0.72f;
        tp.taper = 0.62f;
        tp.branch_angle = 34.0f + (float)(i % 3) * 6.0f;
        tp.angle_variance = 12.0f;
        tp.twist = 137.5f;
        tp.droop = 0.28f;
        tp.curve_noise = 0.35f;
        tp.phototropism = 0.45f;
        tp.lateral_density = 0.8f;
        tp.twig_scale = 0.75f;
        tp.show_leaves = 1;
        // The tree app's 15 is a hero-tree size -- a card is a sprig, and at
        // that scale a sprig is two metres across on a twelve-metre tree.
        tp.leaf_size = 9.0f;
        tp.leaf_density = 8.0f;

        TreeSkeleton skel;
        memset(&skel, 0, sizeof(skel));
        tree_skeleton_build(&skel, &tp);

        g_bark[i] = create_mesh();
        if (!tree_mesh_bark(&skel, &tp, g_bark[i])) {
            free_mesh(g_bark[i]);
            g_bark[i] = NULL;
        } else {
            finalize_mesh(g_bark[i], g_mat_bark, true);
        }

        g_leaf[i] = create_mesh();
        if (!tree_mesh_leaves(&skel, &tp, g_leaf[i])) {
            free_mesh(g_leaf[i]);
            g_leaf[i] = NULL;
        } else {
            finalize_mesh(g_leaf[i], g_mat_leaf, true);
        }
        tree_skeleton_free(&skel);
    }
}

// Fill `items` with up to `count` trees inside [x0, x0+span] x [z0, z0+span],
// drawing from whatever generator the caller seeded.
static int scatter_trees(Placement* items, int count, float x0, float z0, float span) {
    int placed = 0;
    for (int attempt = 0; attempt < count * 12 && placed < count; ++attempt) {
        vec3 p = {0.0f, 0.0f, 0.0f}; // out-param; zeroed for static analysis
        // Tight clumps: groves with real clearings between them. Below the
        // gravel band's own low edge, so no tree stands where the splat has
        // started painting a stream bed.
        if (!sample_ground(0.86f, TREE_MAX_FLOW, 0.0022f, 2.2f, x0, z0, span, p))
            continue;
        float s = TREE_WORLD_SCALE * rnd_range(0.65f, 1.6f);
        glm_vec3_copy(p, items[placed].pos);
        glm_vec3_copy((vec3){s, s, s}, items[placed].scale);
        items[placed].yaw = rnd_range(0.0f, 6.2831853f);
        items[placed].proto = (int)(rnd() * (float)TREE_PROTOTYPES);
        items[placed].key = g_args.no_spatial_sort ? 0u : morton_key(&g_terrain, p[0], p[2]);
        placed++;
    }
    qsort(items, (size_t)placed, sizeof(Placement), placement_cmp);
    return placed;
}

static void build_rock_prototypes(void) {
    for (int i = 0; i < ROCK_PROTOTYPES; ++i) {
        RockParams rp = rock_default_params();
        rp.seed = 7u + (unsigned)i * 13u;
        rp.subdivisions = 3;
        rp.roughness = 0.26f + 0.06f * (float)(i % 3);
        rp.noise_freq = 1.4f + 0.3f * (float)(i % 4);
        g_rocks[i] = create_mesh();
        if (!rock_build_mesh(&rp, g_rocks[i])) {
            free_mesh(g_rocks[i]);
            g_rocks[i] = NULL;
            continue;
        }
        finalize_mesh(g_rocks[i], g_mat_rock, true);
    }

}

static int scatter_rocks(Placement* items, int count, float x0, float z0, float span) {
    int placed = 0;
    for (int attempt = 0; attempt < count * 12 && placed < count; ++attempt) {
        vec3 p = {0.0f, 0.0f, 0.0f}; // out-param; zeroed for static analysis
        // Looser and at a different frequency, so rock fields do not simply
        // mirror the tree groves. No drainage limit: a boulder in a stream bed
        // is where boulders are, and excluding them would leave the one place
        // the terrain is painted gravel conspicuously swept clean.
        if (!sample_ground(0.55f, 1.0f, 0.0035f, 1.3f, x0, z0, span, p))
            continue;
        // Non-uniform scale on purpose: it sits the rock into the ground and,
        // less decoratively, makes each instance's normal matrix differ from
        // mat3(model) -- the lane spec 11.28's fixture was blind to until its
        // props stopped being translation-only boxes.
        float s = rnd_range(0.9f, 5.5f);
        // Sunk a little, but not squashed: a y-scale near half reads as a disc
        // lying on the ground rather than as a boulder sitting in it.
        p[1] -= s * 0.22f;
        glm_vec3_copy(p, items[placed].pos);
        // One draw per statement. Inside an initializer list these three are
        // indeterminately sequenced (C11 6.7.9p23), and each one advances the
        // shared generator -- so the compiler would be free to pick the order,
        // reshaping every rock and shifting the whole stream after it.
        float sx = rnd_range(0.85f, 1.25f);
        float sy = rnd_range(0.7f, 1.05f);
        float sz = rnd_range(0.85f, 1.25f);
        glm_vec3_copy((vec3){s * sx, s * sy, s * sz}, items[placed].scale);
        items[placed].yaw = rnd_range(0.0f, 6.2831853f);
        items[placed].proto = (int)(rnd() * (float)ROCK_PROTOTYPES);
        items[placed].key = g_args.no_spatial_sort ? 0u : morton_key(&g_terrain, p[0], p[2]);
        placed++;
    }
    qsort(items, (size_t)placed, sizeof(Placement), placement_cmp);
    return placed;
}

// --- regions ---------------------------------------------------------------

/*
 * Residency, which is the half of a large world that LOD cannot do.
 *
 * A cluster DAG makes each prop cheaper and a quadtree makes the ground cheaper;
 * neither makes a prop three kilometres away cost nothing, and at constant
 * density there are eighty thousand of them at four kilometres. So the world is
 * cut into squares and only the ones near the camera exist at all -- their props
 * are not in the scene graph and their collision is not in the physics world.
 *
 * A region's scatter is seeded from its own CELL COORDINATES rather than drawn
 * from one global stream, which is what lets it be freed and rebuilt identically.
 * The clump field it rejects against is a function of position, so groves still
 * run across a border: what is per region is the sequence, not the shape.
 *
 * The instance nodes go under the GLOBAL per-prototype groups, not under a node
 * of their own. A group per region would put a foreign mesh between every pair of
 * instances the batcher wants to join, and the run finder breaks on the first one
 * it does not want -- so every run would be length one. What a region keeps
 * instead is the list of nodes it added, which is what it needs to take them out
 * again.
 */
// The nominal side of a region, in world units. Runtime rather than a constant
// because --no-regions is the same machinery with one region over the whole
// domain, which is the configuration every residency arm compares against.
#define REGION_SPAN_DEFAULT 250.0f
static float g_region_span = REGION_SPAN_DEFAULT;
// One region covering everything, always resident: --no-regions, and the reason
// regions_update has an early out rather than an enormous radius.
static bool g_regions_pinned;

// Load inside this, free outside the second, both in world units. The gap is
// hysteresis: without it a camera sitting on the boundary rebuilds a region's
// scatter and its collider every frame.
//
// The default is deliberately larger than a kilometre world's own diagonal, so
// this app's historical configuration has every region resident at all times and
// nothing about it moves. Residency is for the world that needs it, and 11.63's
// own table is the argument: 5,000 props at a kilometre, 80,000 at four.
#define REGION_LOAD_RADIUS 1500.0f
// Hysteresis as a FRACTION of the radius, not a fixed distance: a run that dials
// the radius down to force churn would otherwise inherit a margin wider than the
// world it is churning, and nothing would ever be freed.
#define REGION_UNLOAD_SCALE 1.2f

typedef struct Region {
    int rx, rz;
    bool resident;
    // The instance nodes this region put into the global prototype groups. Just
    // the nodes: the group each one hangs under was stored beside it until
    // free_node started unlinking from SceneNode.parent itself, at which point
    // the pair was a second source of truth for parentage across ~7,000 nodes.
    SceneNode** nodes;
    size_t node_count, node_cap;
    Entity* collider;
    int trees, rocks;
} Region;

static Region* g_regions;
static int g_region_side; // regions per axis; the grid is side x side
static SceneNode* g_group_bark[TREE_PROTOTYPES];
static SceneNode* g_group_leaf[TREE_PROTOTYPES];
static SceneNode* g_group_rock[ROCK_PROTOTYPES];
static PhysicsWorld* g_physics;
static EntityManager* g_entities;
static size_t g_regions_loaded, g_regions_freed;

static Region* region_at(int rx, int rz) {
    if (rx < 0 || rz < 0 || rx >= g_region_side || rz >= g_region_side)
        return NULL;
    return &g_regions[(size_t)rz * (size_t)g_region_side + (size_t)rx];
}

static void region_origin(const Region* r, float* x0, float* z0) {
    *x0 = terrain_world_x(&g_terrain, -g_terrain.extent + g_region_span * (float)r->rx);
    *z0 = terrain_world_z(&g_terrain, -g_terrain.extent + g_region_span * (float)r->rz);
}

// A region's own generator seed, from its cell coordinates and the run's seed.
//
// The hash matters more than it looks: adjacent regions differ by 1 in one
// coordinate, and a seed that varied smoothly with position would give
// neighbouring regions correlated sequences -- visible as the same tree spacing
// repeating across a border.
static unsigned region_seed(int rx, int rz) {
    unsigned h = g_args.seed * 2654435761u;
    h ^= (unsigned)(rx * 73856093) ^ (unsigned)(rz * 19349663);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h ? h : 1u; // the xorshift generator has no state at zero
}

// One region's area share of a WORLD total. TREE_COUNT and ROCK_COUNT are counts
// for the whole domain however large it is, so growing the world spreads the same
// props thinner rather than adding more -- the sum over every region is `total`
// whatever the region size, which is what keeps the scatter arms comparable
// across a world-size change.
//
// Never zero: a region with no props at all is indistinguishable from one that
// failed to load, and the floor costs one prop per region.
static int region_share(int total) {
    float domain = 2.0f * g_terrain.extent;
    float share = (float)total * (g_region_span * g_region_span) / (domain * domain);
    int n = (int)(share + 0.5f);
    return n > 0 ? n : 1;
}

// False if the node could not be remembered, in which case the caller must take
// it back out of the group itself. An untracked node is not merely leaked -- it
// stays in the scene after its region unloads, so it is a prop standing in a
// region that is no longer there, and no later pass can tell it from a live one.
static bool region_track(Region* r, SceneNode* node) {
    if (!grow_array((void**)&r->nodes, &r->node_cap, r->node_count + 1, sizeof(SceneNode*), 64))
        return false;
    r->nodes[r->node_count++] = node;
    return true;
}

// Emit one prototype's share of `items` into its global group, remembering what
// was added so the region can take it back out.
static void region_emit(Region* r, const Placement* items, int count, Mesh* const* protos,
                        int proto_count, SceneNode** groups, const char* prefix) {
    for (int i = 0; i < proto_count; ++i) {
        if (!protos[i])
            continue;
        for (int k = 0; k < count; ++k) {
            if (items[k].proto != i)
                continue;
            if (!groups[i]) {
                char name[40];
                snprintf(name, sizeof(name), "%s_%d", prefix, i);
                groups[i] = make_group(name);
            }
            SceneNode* node = create_node();
            add_mesh_to_node(node, mesh_ref(protos[i]));
            set_node_trs(node, items[k].pos, items[k].yaw, items[k].scale);
            add_child_node(groups[i], node);
            if (!region_track(r, node)) {
                remove_child_node(groups[i], node);
                free_node(node);
                fprintf(stderr, "forest: region %d,%d out of memory at %zu nodes\n", r->rx,
                        r->rz, r->node_count);
                return;
            }
            g_node_count++;
        }
    }
}

static void region_load(Region* r) {
    if (r->resident)
        return;
    float x0, z0;
    region_origin(r, &x0, &z0);

    // Everything below builds something PERSISTENT out of heights -- prop
    // placements and a Jolt BVH -- so the ground it stands on is made exact
    // first. The collider's own build ensures again through build_grid; this
    // call is for the SCATTER, whose flow gate reads a mask that has no pyramid
    // to fall through and whose absence reads as "no water has run here".
    //
    // It also keeps the region digest a function of the world rather than of
    // I/O history, which is what region-return and region-scatter compare.
    if (g_stream)
        terrain_stream_ensure_rect(g_stream, &g_terrain, x0, z0, g_region_span, 0);

    // From its own coordinates, so this is the same scatter every time the
    // region is entered rather than a function of how many regions came before.
    g_rng = region_seed(r->rx, r->rz);

    int tree_n = region_share(TREE_COUNT);
    int rock_n = region_share(ROCK_COUNT);
    Placement* items = malloc((size_t)(tree_n > rock_n ? tree_n : rock_n) * sizeof(Placement));
    if (items) {
        r->trees = scatter_trees(items, tree_n, x0, z0, g_region_span);
        // Bark and leaves go into SEPARATE group sets, from the SAME placements
        // so a tree's two halves stand in one place. A group holding both would
        // put a leaf item between every pair of bark items and every run would be
        // length one.
        region_emit(r, items, r->trees, g_bark, TREE_PROTOTYPES, g_group_bark, "bark");
        region_emit(r, items, r->trees, g_leaf, TREE_PROTOTYPES, g_group_leaf, "leaf");
        probe_collect(items, r->trees);
        probe_low(items, r->trees);

        r->rocks = scatter_rocks(items, rock_n, x0, z0, g_region_span);
        region_emit(r, items, r->rocks, g_rocks, ROCK_PROTOTYPES, g_group_rock, "rock");
        probe_low(items, r->rocks);
        free(items);
    } else {
        // Resident anyway, deliberately: a region that retries every frame under
        // memory pressure thrashes instead of degrading. It gets its collider and
        // no props, which is a walkable hole rather than a fall through the world.
        fprintf(stderr, "forest: region %d,%d could not allocate its scatter\n", r->rx, r->rz);
    }

    // One static body per region. The mesh is CPU geometry whose only job is
    // done once Jolt has copied it into its own BVH.
    Mesh* collider = create_mesh();
    int segments = (int)(g_region_span / (2.0f * g_terrain.extent) * (float)COLLIDER_SEGMENTS + 0.5f);
    if (segments < 2)
        segments = 2;
    if (g_physics && g_entities &&
        terrain_build_collider_region(&g_terrain, x0, z0, g_region_span, segments, collider)) {
        char name[32];
        snprintf(name, sizeof(name), "terrain_%d_%d", r->rx, r->rz);
        Entity* e = create_entity(g_entities, name);
        entity_set_position(e, (vec3){0.0f, 0.0f, 0.0f});
        PhysicsShapeDesc desc = {.type = SHAPE_MESH, .density = 0.0f};
        desc.mesh.vertices = collider->vertices;
        desc.mesh.vertex_count = collider->vertex_count;
        desc.mesh.indices = collider->indices;
        desc.mesh.index_count = collider->index_count;
        if (entity_add_rigid_body(e, g_physics, &desc, MOTION_STATIC, OBJ_LAYER_STATIC))
            r->collider = e;
        else
            destroy_entity(g_entities, e);
    }
    free_mesh(collider);

    r->resident = true;
    g_regions_loaded++;
}

static void region_free(Region* r) {
    if (!r->resident)
        return;
    for (size_t i = 0; i < r->node_count; ++i) {
        // free_node unlinks from the node's own parent, so the group it hangs
        // under does not have to be remembered to take it back out.
        free_node(r->nodes[i]);
        g_node_count--;
    }
    free(r->nodes);
    r->nodes = NULL;
    r->node_count = r->node_cap = 0;
    if (r->collider) {
        destroy_entity(g_entities, r->collider);
        r->collider = NULL;
    }
    r->trees = r->rocks = 0;
    r->resident = false;
    g_regions_freed++;
}

// Squared distance from `p` to a region's square. To the SQUARE and not to its
// centre, so an anchor just inside a region is zero from it whatever its span is.
// Squared horizontal distance from `p` to the region's cell. Y is deliberately
// unbounded -- residency is a question about the ground plane, and a region is a
// column, so a camera high above one is still standing on it.
static float region_dist_sq(const Region* r, const vec3 p) {
    float x0, z0;
    region_origin(r, &x0, &z0);
    AABB cell = {{x0, p[1], z0}, {x0 + g_region_span, p[1], z0 + g_region_span}};
    return aabb_dist_sq(&cell, p);
}

/*
 * Residency against TWO anchors, whichever is nearer, with the load and free
 * radii separated so an anchor sitting on a boundary does not rebuild a region
 * every frame.
 *
 * Two, because the camera and the player are not in the same cell and each owns
 * a different half of the answer. The camera decides what has to be DRAWN. The
 * player decides what has to have COLLISION -- and since collision is per region
 * now, a cell with no body under it is a cell the character falls through. The
 * third-person eye trails by CAM_DISTANCE, so at a small radius those are
 * genuinely different squares.
 */
static void regions_update(const vec3 eye, const vec3 focus) {
    // The stream advances HERE, and this one site is why: residency already
    // takes exactly the two anchors a window wants, and every path that moves
    // regions -- the first pass, the origin-shift rebuild, the per-frame update
    // -- comes through this function. Hanging it off the frame loop instead
    // would leave the rebuild reading windows aimed at where the camera used to
    // be, which is the frame a shift exists to avoid.
    //
    // Before the loads below, so a region that becomes resident this call finds
    // its ground already read rather than ensuring it a second time.
    if (g_stream)
        terrain_stream_update(g_stream, &g_terrain, eye, focus);
    if (!g_regions)
        return;
    if (g_regions_pinned) {
        region_load(&g_regions[0]);
        return;
    }
    float radius = g_args.region_radius > 0.0f ? g_args.region_radius : REGION_LOAD_RADIUS;
    float load2 = radius * radius;
    float free_r = radius * REGION_UNLOAD_SCALE;
    float free2 = free_r * free_r;
    for (int rz = 0; rz < g_region_side; ++rz) {
        for (int rx = 0; rx < g_region_side; ++rx) {
            Region* r = region_at(rx, rz);
            float de = region_dist_sq(r, eye);
            float df = region_dist_sq(r, focus);
            float d2 = de < df ? de : df;
            if (d2 <= load2)
                region_load(r);
            else if (d2 > free2)
                region_free(r);
        }
    }
}

/*
 * Residency and what each region actually placed, in the --water-fft-probe idiom.
 *
 * The instrument exists because both things that can be wrong here are invisible
 * in a frame. A region that reloads with a DIFFERENT scatter looks exactly like a
 * region that reloaded correctly -- one tree is much like another, and the walk
 * that would show it is the walk that made it happen. And a residency that never
 * frees anything renders perfectly; it just runs out of memory a kilometre later.
 *
 * The digest is FNV-1a over the placements' bytes, which is the erosion probe's
 * argument for a digest over a sum: two scatters can agree on their count, their
 * mean position and their prototype histogram while being different scatters.
 */
static unsigned region_digest(const Region* r) {
    const size_t bytes = sizeof(mat4);
    unsigned h = 2166136261u;
    for (size_t i = 0; i < r->node_count; ++i) {
        const SceneNode* n = r->nodes[i];
        const unsigned char* b = (const unsigned char*)n->original_transform;
        for (size_t k = 0; k < bytes; ++k) {
            h ^= b[k];
            h *= 16777619u;
        }
    }
    return h;
}

// The same hash over AUTHORED positions: storage plus the scene's world origin,
// snapped to a whole unit.
//
// A second digest rather than a change to the one above, because the two answer
// different questions and one cannot do both. The raw digest compares two runs at
// the SAME origin and wants the exact bytes -- a sub-unit placement error has to
// fail it. This one compares runs at DIFFERENT origins, where the raw bytes
// cannot match and correctly do not: a shifted run stores every position offset
// by the delta. What a shift owes is that a prop still stands at the same point
// of the world the scene authored, which is what this reads.
//
// SNAPPED for the reason windObjectPhase snaps the position it hashes: the
// reconstruction is a subtraction carrying an ulp at world magnitudes, so two
// runs that placed a prop at the same point can disagree in the low bits. A whole
// unit is orders coarser than that error and orders finer than any misplacement a
// bug would produce.
//
// Composed THROUGH THE PARENT GROUP rather than read off the node's own local
// transform, and that is the difference between this arm working and not.
// regions_rebuild has two halves -- it resets the prototype groups to identity
// and it re-scatters the nodes -- and a node local captures only the second.
// Measured: with the identity reset deleted, which double-shifts every prop that
// reloads, the node-local digest was bit-identical and the arm stayed green;
// composed, it reads 0 of 15 cells matching.
//
// Not global_transform, which would be simpler: the region probe runs before
// apply_transform_to_nodes, so a global is one frame stale and a region loaded
// on the probe frame has none at all. The parent's local IS its world here --
// prototype groups are root children.
static unsigned region_digest_authored(const Region* r) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < r->node_count; ++i) {
        const SceneNode* n = r->nodes[i];
        vec4 local = {n->original_transform[3][0], n->original_transform[3][1],
                      n->original_transform[3][2], 1.0f};
        vec4 world;
        if (n->parent)
            glm_mat4_mulv((vec4*)n->parent->original_transform, local, world);
        else
            glm_vec4_copy(local, world);
        for (int c = 0; c < 3; ++c) {
            float v = floorf(world[c] + g_scene->world_origin[c] + 0.5f);
            // Through memcpy rather than a cast to unsigned char*: the digest
            // above can pun a mat4 because it starts from an array, but punning a
            // scalar local is the aliasing case, and it also reads to cppcheck as
            // a value assigned and never used.
            unsigned char b[sizeof(float)];
            memcpy(b, &v, sizeof(b));
            for (size_t k = 0; k < sizeof(b); ++k) {
                h ^= b[k];
                h *= 16777619u;
            }
        }
    }
    return h;
}

static void region_probe(void) {
    int resident = 0;
    for (int i = 0; i < g_region_side * g_region_side; ++i)
        resident += g_regions[i].resident ? 1 : 0;
    printf("region-probe side=%d span=%.4f resident=%d of %d loaded=%zu freed=%zu nodes=%zu\n",
           g_region_side, (double)g_region_span, resident, g_region_side * g_region_side,
           g_regions_loaded, g_regions_freed, g_node_count);
    for (int rz = 0; rz < g_region_side; ++rz) {
        for (int rx = 0; rx < g_region_side; ++rx) {
            const Region* r = region_at(rx, rz);
            if (!r->resident)
                continue;
            // `authored` last, so a reader parsing this line positionally keeps
            // working. The two are equal only by coincidence at the origin --
            // they hash different things even there, since one takes raw mat4
            // bytes and the other three snapped floats.
            printf("region-probe cell rx=%d rz=%d trees=%d rocks=%d nodes=%zu collider=%d "
                   "digest=%08x authored=%08x\n",
                   rx, rz, r->trees, r->rocks, r->node_count, r->collider ? 1 : 0,
                   region_digest(r), region_digest_authored(r));
        }
    }
}

static void regions_create_sized(PhysicsWorld* physics, EntityManager* em, float span) {
    g_physics = physics;
    g_entities = em;
    g_region_side = (int)((2.0f * g_terrain.extent) / span + 0.5f);
    if (g_region_side < 1)
        g_region_side = 1;
    // The span is then DERIVED back from the count, so the grid tiles the domain
    // exactly. A remainder strip would be ground with no collider under it.
    g_region_span = (2.0f * g_terrain.extent) / (float)g_region_side;
    g_regions = calloc((size_t)g_region_side * (size_t)g_region_side, sizeof(Region));
    if (!g_regions) {
        g_region_side = 0;
        return;
    }
    for (int rz = 0; rz < g_region_side; ++rz)
        for (int rx = 0; rx < g_region_side; ++rx) {
            Region* r = region_at(rx, rz);
            r->rx = rx;
            r->rz = rz;
        }
}

static void regions_create(PhysicsWorld* physics, EntityManager* em) {
    regions_create_sized(physics, em, g_args.region_span > 0.0f ? g_args.region_span
                                                                : REGION_SPAN_DEFAULT);
}

static void regions_create_single(PhysicsWorld* physics, EntityManager* em) {
    regions_create_sized(physics, em, 2.0f * g_terrain.extent);
    g_regions_pinned = true;
}

// Free every region and reload around `eye`, for an origin shift.
//
// A prop's node carries an ABSOLUTE local transform, and the shift has already
// translated the prototype groups those nodes hang under -- which is right for
// instances placed before it and wrong for any placed after, since those come out
// of terrain_world_x against a centre that has already moved. So the groups go
// back to identity and everything resident is placed again.
//
// It costs a full scatter, on a frame that is already rebuilding the terrain and
// reconciling physics, and what comes back is what was there. That last is a
// property of the shift rather than of luck: the seed is the cell index, and
// every field the accept test reads -- height, slope, the erosion masks, the
// clump noise -- is addressed through terrain_to_domain, which subtracts the
// centre the shift just moved. So the region's ground is the same ground and its
// stream is the same stream.
static void regions_rebuild(const vec3 eye, const vec3 focus) {
    if (!g_regions)
        return;
    for (int i = 0; i < g_region_side * g_region_side; ++i)
        region_free(&g_regions[i]);
    for (int i = 0; i < TREE_PROTOTYPES; ++i) {
        if (g_group_bark[i])
            glm_mat4_identity(g_group_bark[i]->original_transform);
        if (g_group_leaf[i])
            glm_mat4_identity(g_group_leaf[i]->original_transform);
    }
    for (int i = 0; i < ROCK_PROTOTYPES; ++i)
        if (g_group_rock[i])
            glm_mat4_identity(g_group_rock[i]->original_transform);
    regions_update(eye, focus);
}

static void regions_free_all(void) {
    if (!g_regions)
        return;
    for (int i = 0; i < g_region_side * g_region_side; ++i)
        region_free(&g_regions[i]);
    free(g_regions);
    g_regions = NULL;
    g_region_side = 0;
    free(g_probe_items);
    g_probe_items = NULL;
    g_probe_count = g_probe_cap = 0;
}

// The lighting when --no-sky takes the atmosphere away. Not merely "skip the
// sky": the sky IS this scene's light -- it supplies the sun, the IBL and the
// aerial perspective -- so removing it without a replacement renders black.
//
// A single directional caster with a flat ambient, which is enough to keep the
// geometry readable and the shadow pass working. It looks worse than the real
// thing on purpose; the flag exists to take the atmosphere's cost out of a
// measurement, not to offer a second look.
static void build_fallback_sun(void) {
    Light* sun = create_light();
    set_light_name(sun, "sun");
    set_light_type(sun, LIGHT_DIRECTIONAL);
    set_light_direction(sun, (vec3){-0.45f, -0.78f, -0.44f});
    set_light_color(sun, (vec3){1.0f, 0.96f, 0.88f});
    set_light_intensity(sun, 3.2f);
    set_light_cast_shadows(sun, true);
    set_light_size(sun, 4.0f, 4.0f);
    add_light_to_scene(g_scene, sun);

    SceneNode* node = create_node();
    set_node_name(node, "sun");
    set_node_light(node, sun);
    add_child_node(g_root, node);

    // No IBL without the sky, so this uniform term is the whole of the fill.
    // It defaults to zero, which would leave every shadowed surface black.
    // Cool, because the thing it stands in for is skylight.
    glm_vec3_copy((vec3){0.45f, 0.55f, 0.75f}, g_scene->ambient_radiance);
    g_scene->render_skybox = false;
}

static void build_sky_and_sun(Engine* engine) {
    SkyAtmosphere* sky = create_sky_atmosphere();
    IBLResources* ibl = create_ibl_resources();
    if (!sky || !ibl)
        return;

    // The CLI wins, and must land before the bake: the LUTs are a function of
    // where the sun is.
    // Low on purpose. A high sun lights the fog uniformly from above and the
    // medium reads as haze; a low one rakes through it, so the anisotropy has a
    // direction to work along and the ridge lines separate into layers.
    sky->sun_elevation_deg = g_args.sun_elevation > -900.0f ? g_args.sun_elevation : 15.0f;
    sky->sun_azimuth_deg = g_args.sun_azimuth > -900.0f ? g_args.sun_azimuth : 152.0f;
    // One world unit is one metre here, which is what makes aerial perspective
    // read correctly over a kilometre instead of hazing the near ground.
    sky->world_units_per_km = 1000.0f;
    sky->aerial_enabled = !g_args.no_aerial;
    sky_update_sun_dir(sky);

    if (sky_bake_static_luts(sky, engine) != 0 || sky_bake(sky, ibl, engine) != 0)
        return;

    g_scene->sky = sky;
    g_scene->ibl = ibl;
    g_scene->render_skybox = true;
    g_scene->skybox_brightness = 1.0f;
    g_scene->skybox_ground_projection = false;

    Light* sun = create_light();
    set_light_name(sun, "sun");
    set_light_type(sun, LIGHT_DIRECTIONAL);
    set_light_cast_shadows(sun, true);
    set_light_size(sun, 4.0f, 4.0f);
    sky->sun_light = sun;
    // Lower than the tree app's 10: that app frames one subject against a
    // backdrop and can afford to blow its highlights, where a whole terrain of
    // mid-albedo surfaces facing the sun clips instead.
    sky->sun_base_intensity = 6.0f;
    sky_apply_sun_to_light(sky);
    add_light_to_scene(g_scene, sun);

    SceneNode* node = create_node();
    set_node_name(node, "sun");
    set_node_light(node, sun);
    add_child_node(g_root, node);
}

// --- callbacks -------------------------------------------------------------

// The one absolute this app owns that no library can reach (spec 11.62): the
// terrain's HEIGHT FUNCTION, which is what the collider build, the scatter, the
// water bed and the camera's floor clamp all ask where the ground is. Its mesh
// moved with the graph, so leaving this behind separates the surface the player
// walks on from the one that gets drawn.
//
// Everything else -- physics bodies, characters, their entities' cached
// positions -- is the game framework's own state and is handled there.
static void forest_on_origin_shift(const vec3 delta, void* ctx) {
    game_on_origin_shift_default(delta, ctx);
    g_terrain.center[0] -= delta[0];
    g_terrain.center[1] -= delta[2];
    // A patch bakes world coordinates into its vertices, so moving the terrain
    // under the cache makes every one of them describe ground that is no longer
    // there. There is no correction to apply, so they are rebuilt -- and rebuilt
    // HERE rather than at the next descent, because this runs before the shadow
    // pass and the GI captures.
    Game* game = ctx;
    Camera* camera = game && game->engine ? game->engine->camera : NULL;
    vec3 eye = {0.0f, 0.0f, 0.0f};
    if (camera)
        glm_vec3_copy(camera->position, eye);
    if (g_terrain_qt)
        terrain_quadtree_rebuild(g_terrain_qt, eye);
    // Same hazard, same repair: an instance placed after the shift comes out of a
    // centre that has already moved, under a group the shift has already
    // translated. The player has been shifted too, so its cell is still the one
    // that needs a collider under it.
    vec3 focus;
    glm_vec3_copy(g_player ? g_player->position : eye, focus);
    regions_rebuild(eye, focus);
}

static void on_init(Game* game) {
    Engine* engine = game->engine;
    g_pbr = get_engine_shader_program_by_name(engine, "pbr");

    g_scene = create_scene();
    g_root = create_node();
    set_node_name(g_root, "root");
    set_scene_root_node(g_scene, g_root);
    game_set_scene(game, g_scene);

    g_terrain = terrain_default_params();
    g_terrain.seed = g_args.seed;
    // Everything else here places itself relative to the terrain, so this one
    // assignment is what moves the world (spec 11.62). The offset is
    // MATERIALISED -- it lands in every vertex, every collider triangle and
    // every scatter position as an fp32 world coordinate -- which is the point:
    // what this measures is what a large world costs before an origin shift.
    g_terrain.center[0] = g_terrain.center[1] = g_args.world_offset;
    // Taller and broader than the library default: at 55 over a 250-unit
    // wavelength the ground reads as flat from anywhere a person stands, and
    // terrain that reads flat gives LOD and culling nothing to work with.
    g_terrain.height = 95.0f;
    g_terrain.base_freq = 0.0026f;
    g_terrain.octaves = 6;
    if (g_args.terrain_extent > 0.0f) {
        // The noise is left alone: what changes is how much of it there is, which
        // is the only reading that isolates what a larger world costs from what a
        // different landscape costs.
        g_terrain.extent = g_args.terrain_extent;
        // The fixed grid has no answer to a bigger domain but more tiles, so it
        // gets them -- otherwise --no-quadtree would be comparing a quadtree that
        // held its cell size against a grid that quietly stopped.
        g_terrain.tiles = (int)((2.0f * g_terrain.extent) / 125.0f + 0.5f);
        if (g_terrain.tiles < 1)
            g_terrain.tiles = 1;
    }
    if (!g_args.no_island) {
        // Half the half-extent inland before the ground starts falling, so the
        // shore is a broad shoal rather than a cliff into the sea, and a sea
        // floor deeper than the fbm's own amplitude so the rim is under water
        // whatever the noise does there.
        g_terrain.island_start = ISLAND_START;
        g_terrain.island_depth = ISLAND_DEPTH;
        // The fbm is symmetric about zero, so sea level at zero would flood half
        // the interior. Below it, and the number is what decides how much of the
        // island is land: at -35 against a 95-unit amplitude the ground is dry
        // wherever the noise is above about a third of the way down.
        // Not asked for where water is refused anyway (see the create_water call
        // below): the island still SHAPES there, and a warning about a flag the
        // app set for itself is noise.
        if (!world_leaves_origin())
            g_args.water = 1;
        if (g_args.water_level == 0.0f)
            g_args.water_level = ISLAND_SEA_LEVEL;
    }
    g_rng = g_args.seed * 2654435761u + 1u;
    noise_perm_init(&g_clump, g_args.seed ^ 0x5bf03635u);

    // Before anything reads a height. The scatter, the collider and the tiles all
    // have to see the same surface, and they see it through g_terrain.
    // A loaded field wins over a baked one: the file is a statement about what
    // the terrain IS, and re-eroding it would be eroding someone's finished work.
    // Said out loud, because every --erode-* flag also SETS --erode, so a command
    // line asking to bake and save alongside a --heightmap would otherwise get no
    // bake, no file and no explanation.
    // A STREAMED field wins over both, for the same reason a loaded one wins
    // over a bake and one step further along: the file already holds the
    // pyramid, so building either of the others would be producing a surface
    // nothing is going to read.
    if (g_args.terrain_stream && (g_args.heightmap || g_args.erode))
        fprintf(stderr, "forest: --terrain-stream wins over --heightmap and --erode; neither "
                        "is loaded and any save of them is skipped\n");
    else if (g_args.heightmap && g_args.erode)
        fprintf(stderr, "forest: --heightmap wins over --erode; the bake and any --erode-save "
                        "are skipped\n");
    if (g_args.terrain_stream) {
        float radius = g_args.region_radius > 0.0f ? g_args.region_radius : REGION_LOAD_RADIUS;
        float span = g_args.region_span > 0.0f ? g_args.region_span : REGION_SPAN_DEFAULT;
        load_stream(radius + 2.0f * span);
    } else if (g_args.heightmap)
        load_heightfield();
    else if (g_args.erode)
        bake_erosion();
    // Here rather than inside each of those, so a third way of getting a field
    // cannot arrive without one. The levels are copies of what is in the field
    // at this moment, so this has to be the last thing that touches it.
    //
    // A streamed field is exempt and not merely skipped: its levels came out of
    // the file already, and rebuilding them would need the whole surface in
    // memory, which is the thing streaming exists not to do.
    if (g_terrain.field == &g_field) {
        int levels = terrain_field_build_pyramid(&g_field);
        printf("Terrain field: %d level(s), finest cell %.3f units\n", levels,
               (double)terrain_field_cell(g_terrain.extent, g_field.res));
    }
    // After the pyramid, because the levels are what a stream file stores.
    if (g_args.terrain_stream_save && g_terrain.field == &g_field)
        save_stream();
    if (g_args.height_probe)
        terrain_height_probe(&g_terrain);

    // Albedo white on the textured materials: the shader multiplies factor by
    // map, so any factor below one darkens the whole range.
    g_mat_terrain = make_material("terrain", (vec3){1.0f, 1.0f, 1.0f}, 0.92f, 0.0f);
    g_mat_bark = make_material("bark", (vec3){1.0f, 1.0f, 1.0f}, 1.0f, 0.0f);
    g_mat_leaf = make_material("leaf", (vec3){0.85f, 1.0f, 0.8f}, 1.0f, 0.0f);
    // Cutout, two-sided, and allowed into the shadow map -- foliage_shadows is
    // what dapples the ground, and without alpha_mode the cards are solid quads.
    g_mat_leaf->alpha_mode = ALPHA_MASK;
    g_mat_leaf->alphaCutoff = LEAF_ALPHA_CUTOFF;
    g_mat_leaf->doubleSided = true;
    g_mat_leaf->foliage_shadows = 1;
    // Thin leaves transmit; without it a backlit canopy reads as opaque plastic.
    g_mat_leaf->subsurface = 0.55f;
    // Wind. Both prototypes come from tree_gen, which already writes the UV1
    // branch phase and flex weight the vegetation modes read, so this is a
    // material field and not new geometry. Modest against a 125-unit trunk: the
    // response multiplies the scene strength, and a canopy that travels metres
    // reads as a storm rather than as air moving.
    g_mat_bark->wind_response = 0.45f;
    g_mat_bark->wind_mode = 1; // whole-trunk lean plus per-branch sway
    g_mat_leaf->wind_response = 0.7f;
    g_mat_leaf->wind_mode = 2; // that, plus the card flutter
    // Dark wet stone. Rock is the only light NEUTRAL surface in a scene of dark
    // foliage, so anything near a realistic granite albedo reads as white against
    // it however the lighting is balanced -- the surrounding palette sets what
    // this can be, not the material on its own.
    g_mat_rock = make_material("rock", (vec3){0.085f, 0.082f, 0.078f}, 0.88f, 0.0f);

    bake_vegetation_textures(g_scene);
    bake_terrain_layers(g_scene);

    // A breeze across the valley. phase_variation is what makes TREE_COUNT copies
    // of TREE_PROTOTYPES meshes look like trees rather than one tree drawn two
    // thousand times: wind is evaluated in object space, so without it every
    // instance sways on the same beat.
    Wind* wind = create_wind("valley breeze");
    if (wind) {
        glm_vec3_copy((vec3){0.82f, 0.0f, 0.57f}, wind->direction);
        wind->strength = 3.0f;
        wind->speed = 0.9f;
        wind->gust_frequency = 0.18f;
        wind->gust_amount = 0.5f;
        wind->turbulence = 0.35f;
        wind->phase_variation = 1.0f;
        set_scene_wind(g_scene, wind);
    }

    PhysicsConfig pc = physics_default_config();
    PhysicsWorld* physics = create_physics_world(&pc);
    game_set_physics_world(game, physics);
    EntityManager* em = create_entity_manager(game);
    game_set_entity_manager(game, em);

    build_terrain();
    build_tree_prototypes();
    build_rock_prototypes();
    if (g_args.no_regions) {
        // One region covering the whole domain, always resident. The scatter and
        // the collider then run exactly once, which is what this app did before
        // residency existed and what every residency arm compares against.
        regions_create_single(physics, em);
    } else {
        regions_create(physics, em);
    }
    // The first residency pass, here rather than at the first frame: the water
    // bed, the player spawn and the initial camera all want a world that exists.
    // The spawn is the domain centre, so it is the focus anchor whether or not a
    // camera was pinned -- the player must land on ground that has a collider.
    vec3 spawn = {g_terrain.center[0], 0.0f, g_terrain.center[1]};
    vec3 home;
    glm_vec3_copy(spawn, home);
    if (g_args.cam_set)
        glm_vec3_copy(g_args.cam_eye, home);
    regions_update(home, spawn);
    if (g_args.scatter_probe && g_probe_items)
        scatter_probe(g_probe_items, (int)g_probe_count);
    printf("Trees and rocks: %d prototypes, %d region(s) of %d resident\n",
           TREE_PROTOTYPES + ROCK_PROTOTYPES, (int)g_regions_loaded,
           g_region_side * g_region_side);
    // The creation reference is NOT released here, unlike the one-shot scatter
    // this replaced. A region that loads three seconds from now takes its own
    // ref from these pointers, so dropping the last one when the near regions
    // happen not to use a prototype would free the mesh and leave the pointer
    // behind. They are released at shutdown instead.
    if (g_args.no_sky)
        build_fallback_sun();
    else
        build_sky_and_sun(engine);

    // Flood it. This is the whole point of the water system's height provider:
    // terrain_height_at is a pure function of (params, x, z), so it satisfies
    // WaterHeightFn directly and the surface can shoal against real terrain with
    // no heightmap stored anywhere and no data copied.
    // Water's bed domain is a half-size about the STORAGE origin with no centre of
    // its own, so a world placed elsewhere shoals against terrain that is not
    // under it. Refused rather than approximated: the two together produce a
    // plausible frame that is wrong everywhere.
    if (g_args.water && world_leaves_origin())
        fprintf(stderr, "forest: --water needs the world at the origin; --world-offset / "
                        "--origin-shift-distance / --origin-shift-at skip it\n");
    if (g_args.water && !world_leaves_origin()) {
        Water* water = create_water();
        if (water) {
            water->level = g_args.water_level;
            // The SHOALING BED's domain, which is the terrain square, because that is
            // exactly where a bed exists to shoal against. It does not bound the drawn
            // surface, which reaches the horizon regardless (spec 11.35) -- past the
            // terrain the bed field reads its edge and the sea is open water, which is
            // what a flooded island should look like from the shore anyway.
            water->extent = g_terrain.extent;
            water->height_at = forest_bed_height;
            water->height_ctx = &g_terrain;
            g_scene->water = water;
        }
    }

    // Standing on the surface, not dropped onto it. The capsule's origin sits
    // half_height + radius above whatever it rests on, so spawning any higher
    // means the first second of every run is a fall -- which reads as the
    // character sliding before it settles.
    const float capsule_rest = 0.9f + 0.4f;
    float spawn_x = g_terrain.center[0], spawn_z = g_terrain.center[1];
    float spawn_y = terrain_height_at(&g_terrain, spawn_x, spawn_z) + capsule_rest + 0.02f;
    g_player = create_entity(em, "player");
    entity_set_position(g_player, (vec3){spawn_x, spawn_y, spawn_z});
    CharacterControllerConfig cc = character_controller_default_config();
    cc.capsule_radius = 0.4f;
    cc.capsule_half_height = 0.9f;
    cc.step_height = 0.5f;
    // 45, not 55: terrain this steep is common, and a controller willing to
    // stand on a 55-degree face spends its time creeping down one.
    cc.max_slope_angle = 45.0f;
    // Longer than the 0.5 default, because the ground here is a triangle mesh
    // and a capsule crossing a convex edge otherwise leaves it for a frame,
    // loses ground contact, and starts falling mid-stride.
    cc.stick_to_floor_distance = 1.2f;
    cc.penetration_recovery_speed = 1.5f;
    entity_add_character_controller(g_player, physics, &cc);

    physics_world_optimize(physics);

    scene_set_origin_callback(g_scene, forest_on_origin_shift, game);
    g_scene->origin_shift_distance = g_args.origin_shift_distance;

    // Shadows sized AND placed to the terrain, not left at the library defaults
    // (ortho 2000 about the origin). The scene-fit map covers ortho_size about
    // its centre, so a terrain that is not at the origin needs both.
    ShadowSystem* ss = g_scene->shadow_system;
    if (ss) {
        ss->enabled = true;
        ss->scene_center[0] = g_terrain.center[0];
        ss->scene_center[2] = g_terrain.center[1];
        ss->ortho_size = g_terrain.extent;
        ss->near_plane = 0.5f;
        ss->far_plane = g_terrain.extent * 6.0f;
        // The slices stop at half the island rather than at the camera's 2000,
        // which is what they were fitted across and is a view distance, not a
        // shadow one. Across the far clip cascade 1 fitted a 967.8-unit box
        // against the outermost cascade's 1000 -- a third of the pass spent
        // re-drawing the ground the third cascade already had at the same
        // density. The trees whose shadows anyone reads are inside this.
        ss->shadow_distance = g_terrain.extent * 0.5f;
        ss->cascade_count = SHADOW_CASCADES;
        ss->pcss_enabled = true;
        ss->pcss_softness = 1.2f;
    }

    Camera* camera = create_camera();
    // 0.5 / 2000 rather than the 0.1 / 1000 default: a kilometre of terrain
    // needs the far plane, and 0.1 near against it is a 20000:1 depth ratio that
    // z-fights across the whole distance.
    set_camera_perspective(camera, glm_rad(58.0f), 0.5f, 2000.0f);
    set_camera_position(camera, (vec3){spawn_x, spawn_y + 6.0f, spawn_z + 16.0f});
    set_camera_look_at(camera, (vec3){spawn_x, spawn_y, spawn_z});
    set_camera_up_vector(camera, (vec3){0.0f, 1.0f, 0.0f});
    set_engine_camera(engine, camera);
    set_engine_camera_mode(engine, CAMERA_MODE_FREE);

    // Pinned rather than adaptive: auto-exposure is the top determinism hazard
    // for anything compared across builds, and every arm here reads a frame or a
    // counter from this app.
    engine->exposure.automatic = false;
    engine->exposure.multiplier = 1.8f;

    if (g_args.render_mode > 0)
        engine->current_render_mode = (RenderMode)g_args.render_mode;

    if (g_args.no_lod)
        engine->lod_enabled = false;
    if (g_args.no_instancing)
        engine->instancing_enabled = false;
    if (g_args.no_morph)
        engine->morph_enabled = false;
    if (g_args.no_layers_vt)
        engine->layers_vt_enabled = false;
    if (g_args.layers_vt_res > 0)
        engine->layers_vt_res = g_args.layers_vt_res;
    if (g_args.no_layers_vt_pages)
        engine->layers_vt_pages_enabled = false;
    if (g_args.no_layers_vt_feedback)
        engine->layers_vt_feedback_enabled = false;
    if (g_args.layers_vt_page_slots > 0)
        engine->layers_vt_page_slots = g_args.layers_vt_page_slots;
    if (g_args.layers_vt_page_budget > 0)
        engine->layers_vt_page_budget = g_args.layers_vt_page_budget;
    if (g_args.layers_vt_probe > 0)
        engine->layers_vt_probe_interval = g_args.layers_vt_probe;
    if (g_args.no_sort_opaque)
        engine->opaque_sort_enabled = false;
    if (g_args.depth_prepass)
        engine->depth_prepass_enabled = true;
    if (g_args.lod_bias > 0.0f)
        engine->lod_bias = g_args.lod_bias;

    // Volumetric fog, on by default, and sized to the world rather than left at
    // the struct defaults -- fog_far bounds the froxel volume, so a value short
    // of the far plane simply stops the medium partway across the terrain and
    // leaves a visible edge where it ends.
    //
    // Scale matters more than density here: one unit is one metre, so the
    // extinction is per metre and the numbers are small.
    if (engine->postfx && !g_args.no_fog) {
        PostFX* fx = engine->postfx;
        fx->fog_enabled = true;
        fx->fog_density = 0.0022f;
        fx->fog_height_falloff = 42.0f; // thick in the valleys, thin off the ridges
        fx->fog_floor_y = -30.0f;
        fx->fog_near = 0.0f; // derive from fog_far
        fx->fog_far = 1400.0f;
        // Forward-scattering, so looking toward the sun lights the medium up and
        // looking away leaves it flat -- most of what reads as "moody".
        fx->fog_anisotropy = 0.72f;
        fx->fog_sun_boost = 2.2f;
        glm_vec3_copy((vec3){0.030f, 0.038f, 0.055f}, fx->fog_ambient);
    }

    set_engine_show_gui(engine, !engine->headless);
    set_engine_show_fps(engine, !engine->headless);

    printf("Forest: %zu distinct meshes, %zu prototype triangles, %zu nodes\n", g_distinct_meshes,
           g_prototype_tris, g_node_count);
    printf("Forest: %zu LOD chains built, %zu refused\n", g_chains_built, g_chains_refused);
    printf("Forest: %zu clustered meshes, %zu clusters\n", g_clustered_meshes, g_clusters_built);
}

// The lattice the diagnostic shift snaps to. Matches what the engine derives
// from ORIGIN_SHIFT_DISTANCE's default so the hand-driven and automatic paths
// land on the same point; the snap itself belongs to the engine.
#define ORIGIN_SHIFT_LATTICE 256.0f

static void on_update(Game* game, double dt) {
    // Diagnostic (--origin-shift-at). Before the player guard, and `>=` with a
    // latch rather than `==`: this runs on the FIXED-step clock, which may take
    // zero or two steps in a frame, so an equality test can miss the frame it
    // was asked for entirely.
    static bool origin_shift_fired;
    if (g_args.origin_shift_at > 0 && !origin_shift_fired && game->engine &&
        game->engine->total_frames >= (size_t)g_args.origin_shift_at) {
        origin_shift_fired = true;
        engine_recentre_on_camera(game->engine, ORIGIN_SHIFT_LATTICE);
    }

    if (!g_player)
        return;
    CharacterController* cc = entity_get_character_controller(g_player);
    if (!cc)
        return;

    vec3 input_dir;
    input_wasd_direction(&game->input, input_dir);

    // Scripted walk (--walk), which is the only way a headless run can cross a
    // region boundary: residency follows the camera and the camera follows the
    // player, and no key is ever pressed. Turns about-face at the halfway frame
    // WHEN THERE IS ONE -- --walk without --frames has no midpoint to turn at and
    // walks forward until the window closes, which is the interactive use. With a
    // frame count the run is a ROUND TRIP: what leaves has to come back, which is
    // what makes the determinism and the leak readable off one capture.
    if (g_args.walk > 0.0f && game->engine) {
        input_dir[2] = -1.0f;
        input_dir[0] = 0.0f;
        if (g_args.frames > 0 && game->engine->total_frames * 2 >= (size_t)g_args.frames)
            input_dir[2] = 1.0f;
    }

    // Rotated into the camera's yaw. gametest's is world-axis-aligned, which
    // stops making sense the moment the camera is not facing -Z.
    vec3 fwd = {sinf(g_cam_yaw), 0.0f, cosf(g_cam_yaw)};
    vec3 right = {-cosf(g_cam_yaw), 0.0f, sinf(g_cam_yaw)};

    vec3 vel;
    character_controller_get_velocity(cc, vel);

    const float speed = g_args.walk > 0.0f
                            ? g_args.walk
                            : (input_key_down(&game->input, GLFW_KEY_LEFT_SHIFT) ? 16.0f : 7.0f);
    vec3 move = {0.0f, 0.0f, 0.0f};
    glm_vec3_muladds(fwd, -input_dir[2] * speed, move);
    glm_vec3_muladds(right, input_dir[0] * speed, move);

    bool grounded = character_controller_is_grounded(cc);

    // Horizontal velocity is SET, not accumulated, so releasing the keys stops
    // the character rather than coasting. On a slope that is not enough on its
    // own -- see the vertical term below, which is what actually caused the
    // skating.
    vel[0] = move[0];
    vel[2] = move[2];

    if (grounded) {
        // Zero, not a small negative. Any downward velocity left on a grounded
        // character is resolved by ExtendedUpdate ALONG the surface, so on a
        // slope it becomes downhill travel: measured at 0.36 m/s on a 10-degree
        // face from a -2 m/s residual, which is 2*sin(10) and exactly the
        // skating this was meant to cure. Ground adherence is
        // stick_to_floor_distance's job, not a velocity's.
        vel[1] = 0.0f;
    } else {
        vel[1] -= 22.0f * (float)dt;
    }

    if (input_key_pressed(&game->input, GLFW_KEY_SPACE) && grounded)
        vel[1] = 9.5f;

    character_controller_set_velocity(cc, vel);

    // Whether the character is at rest is not something the frame shows -- the
    // camera follows it, so drift and stillness look identical from inside.
    if (g_args.trace_player) {
        static int step;
        if (step++ % 30 == 0) {
            vec3 pos, gn;
            character_controller_get_position(cc, pos);
            character_controller_get_ground_normal(cc, gn);
            printf("player t=%5.2f pos %8.2f %8.2f %8.2f  vel %6.2f %6.2f %6.2f  "
                   "grounded %d  ground_n.y %.3f  terrain %.2f\n",
                   (double)step / 60.0, (double)pos[0], (double)pos[1], (double)pos[2],
                   (double)vel[0], (double)vel[1], (double)vel[2], grounded ? 1 : 0, (double)gn[1],
                   (double)terrain_height_at(&g_terrain, pos[0], pos[2]));
        }
    }
}

static void on_render(Game* game, double alpha) {
    (void)alpha;
    Engine* engine = game->engine;
    if (!g_scene || !g_scene->root_node)
        return;

    Camera* camera = engine->camera;
    if (camera) {
        if (g_args.cam_set) {
            // Re-pinned every frame, so it has to be expressed in the frame the
            // world is CURRENTLY stored in: the flag is a world coordinate
            // somebody typed, which is authoring space, and after an origin shift
            // that is no longer the same as storage. Writing the absolute back
            // each frame would silently undo the shift for the camera alone and
            // leave it looking at where the world used to be.
            vec3 eye, target;
            glm_vec3_sub(g_args.cam_eye, g_scene->world_origin, eye);
            glm_vec3_sub(g_args.cam_target, g_scene->world_origin, target);
            set_camera_position(camera, eye);
            set_camera_look_at(camera, target);
        } else if (g_player) {
            if (input_mouse_down(&game->input, GLFW_MOUSE_BUTTON_LEFT) ||
                input_mouse_down(&game->input, GLFW_MOUSE_BUTTON_RIGHT)) {
                double dx = 0.0, dy = 0.0;
                input_mouse_delta(&game->input, &dx, &dy);
                g_cam_yaw -= (float)dx * 0.005f;
                g_cam_pitch += (float)dy * 0.005f;
            }
            if (input_key_down(&game->input, GLFW_KEY_Q))
                g_cam_yaw += 0.03f;
            if (input_key_down(&game->input, GLFW_KEY_E))
                g_cam_yaw -= 0.03f;
            if (g_cam_pitch < -0.2f)
                g_cam_pitch = -0.2f;
            if (g_cam_pitch > 1.25f)
                g_cam_pitch = 1.25f;

            vec3 target;
            glm_vec3_copy(g_player->position, target);
            target[1] += 1.5f;

            float ch = cosf(g_cam_pitch);
            vec3 eye = {target[0] - sinf(g_cam_yaw) * ch * CAM_DISTANCE,
                        target[1] + sinf(g_cam_pitch) * CAM_DISTANCE,
                        target[2] - cosf(g_cam_yaw) * ch * CAM_DISTANCE};
            // Never below the ground the character is standing on.
            float floor_y = terrain_height_at(&g_terrain, eye[0], eye[2]) + 1.0f;
            if (eye[1] < floor_y)
                eye[1] = floor_y;
            set_camera_position(camera, eye);
            set_camera_look_at(camera, target);
        }
        // Without both of these the view matrix keeps whatever it had; nothing
        // else in a game-framework app writes it.
        update_engine_camera_lookat(engine);
        update_engine_camera_perspective(engine);
    }

    // The descent, before the transform walk that gives a newly attached patch
    // its global transform and before the draw list is asked what to draw.
    // Against the camera the frame will actually use, which is why it is here and
    // not in the fixed-step update: at a fixed step the selection would lag the
    // view by up to a frame, and the morph is a function of exactly this eye.
    if (camera) {
        // Residency first: the quadtree hangs its patches under a node of its
        // own and does not care, but a region that loads here has to be in the
        // graph before the transform walk below reaches it.
        vec3 focus;
        glm_vec3_copy(g_player ? g_player->position : camera->position, focus);
        regions_update(camera->position, focus);
        if (g_terrain_qt) {
            terrain_quadtree_update(g_terrain_qt, camera->position);
            if (g_args.quadtree_probe && engine->total_frames + 1 == (size_t)g_args.frames)
                terrain_quadtree_probe(g_terrain_qt);
        }
        if (g_args.region_probe && engine->total_frames + 1 == (size_t)g_args.frames)
            region_probe();
        if (g_stream && g_args.terrain_stream_probe > 0) {
            bool final = engine->total_frames + 1 == (size_t)g_args.frames;
            if (final || engine->total_frames % (size_t)g_args.terrain_stream_probe == 0)
                terrain_stream_probe(g_stream, &g_terrain, engine->total_frames, final);
        }
    }

    Transform t = {.position = {0, 0, 0}, .rotation = {0, 0, 0}, .scale = {1, 1, 1}};
    reset_and_apply_transform(&engine->model_matrix, &t);
    apply_transform_to_nodes(g_scene->root_node, engine->model_matrix);

    render_current_scene(engine);
}

static void on_shutdown(Game* game) {
    // run_game does not report; the render app does this at its own exit. Here
    // because the whole app exists to be read off these tables.
    if (game && game->engine)
        profiler_report(game->engine->profiler);
    // What the terrain cost, which is the one thing about this scene that is not
    // a fixed number any more: the quadtree's selection is a function of where
    // the camera ended up. Printed rather than left to the probe because anything
    // comparing two runs has to be able to separate terrain from props, and the
    // submission table sums them.
    if (g_terrain_qt) {
        TerrainQuadtreeStats qs;
        terrain_quadtree_stats(g_terrain_qt, &qs);
        printf("Forest terrain: quadtree %d levels, %d patches selected, %d resident, "
               "%zu triangles\n",
               qs.levels, qs.selected, qs.resident, qs.triangles);
    } else {
        printf("Forest terrain: fixed grid, %d tiles\n", g_terrain.tiles * g_terrain.tiles);
    }

    // Before the scene, whose root still points at the group these hang under:
    // a patch node freed here has to be detached from that group first, and the
    // quadtree is the only thing that knows which of them are attached.
    free_terrain_quadtree(g_terrain_qt);
    g_terrain_qt = NULL;
    g_terrain_group = NULL;
    // Before the scene: a region holds instance nodes parented under groups the
    // scene root owns, and they have to be detached before either is freed.
    regions_free_all();
    // The prototypes' creation reference, held for the process lifetime so a
    // region loading at any moment could take one from it.
    for (int i = 0; i < TREE_PROTOTYPES; ++i) {
        free_mesh(g_bark[i]);
        free_mesh(g_leaf[i]);
    }
    for (int i = 0; i < ROCK_PROTOTYPES; ++i)
        free_mesh(g_rocks[i]);
    // g_terrain borrows this, so it outlives every consumer by construction and
    // is released only once nothing can ask for a height again.
    g_terrain.field = NULL;
    terrain_field_free(&g_field);
    terrain_stream_free(g_stream);
    g_stream = NULL;
}

// --- entry point -----------------------------------------------------------

static void print_usage(const char* argv0) {
    fprintf(stderr, "Usage: %s [options]\n", argv0);
    fprintf(stderr, "  -x, --headless          Hidden window (capture / CI)\n");
    fprintf(stderr, "  -f, --frames N          Exit after N frames\n");
    fprintf(stderr, "  -S, --screenshot PATH   Save the final frame as PPM\n");
    fprintf(stderr, "  -W, -H <n>              Window size\n");
    fprintf(stderr, "      --profiler          Per-pass timing + submission counters\n");
    fprintf(stderr, "      --no-lod            Draw every mesh at LOD level 0\n");
    fprintf(stderr, "      --no-clusters       Build LOD chains instead of cluster DAGs\n");
    fprintf(stderr, "      --no-quadtree       Fixed terrain tile grid, not the CDLOD quadtree\n");
    fprintf(stderr, "      --no-instancing     One draw per mesh\n");
    fprintf(stderr,
            "      --no-layers-vt      Per-texel layered blend instead of the composite cache\n");
    fprintf(stderr, "      --layers-vt-res N   Composite-cache resolution override (diagnostic)\n");
    fprintf(stderr, "      --no-layers-vt-pages      Fallback atlas alone (no paged near field)\n");
    fprintf(stderr, "      --no-layers-vt-feedback   Page residency on prediction alone\n");
    fprintf(stderr, "      --layers-vt-page-slots N  Physical page slots in use (diagnostic)\n");
    fprintf(stderr, "      --layers-vt-probe N       Print page residency every N frames\n");
    fprintf(stderr, "      --no-sort-opaque    Draw opaques in graph order\n");
    fprintf(stderr, "      --depth-prepass     Depth-only pass before shading\n");
    fprintf(stderr, "      --taa               TAA headless too (diagnostic)\n");
    fprintf(stderr, "      --msaa <n>          Sample count, overriding the TAA/headless policy\n");
    fprintf(stderr, "      --headless-jitter   Sub-pixel jitter headless\n");
    fprintf(stderr, "      --no-sky            Plain directional rig, no atmosphere\n");
    fprintf(stderr, "      --no-fog            Disable the volumetric fog\n");
    fprintf(stderr, "      --no-aerial         Keep the sky, drop aerial perspective\n");
    fprintf(stderr, "      --sun-elevation <d> Sun elevation in degrees\n");
    fprintf(stderr, "      --sun-azimuth <d>   Sun azimuth in degrees\n");
    fprintf(stderr, "      --no-spatial-sort   Scatter without Morton ordering\n");
    fprintf(stderr, "      --trace-player      Log position, velocity and ground state\n");
    fprintf(stderr, "      --render-mode N     1 = normals, 6 = albedo\n");
    fprintf(stderr, "      --lod-bias F        >1 holds detail longer\n");
    fprintf(stderr, "      --water             Flood the terrain (spec 11.32)\n");
    fprintf(stderr, "      --water-level <f>   Still-water world Y (implies --water)\n");
    fprintf(stderr, "      --erode             Bake a heightfield and erode it\n");
    fprintf(stderr, "      --erode-res <n>     Field resolution (default 513)\n");
    fprintf(stderr, "      --erode-iterations <n>  Sim steps (default 220)\n");
    fprintf(stderr, "      --erode-workers <n> Pin the thread count (0 = machine)\n");
    fprintf(stderr, "      --terrain-erosion-probe  Print the bake's own numbers\n");
    fprintf(stderr, "      --erode-save <p>    Write the baked field as .r16\n");
    fprintf(stderr, "      --heightmap <p>     Load a field (.r16 / 16-bit PNG) instead\n");
    fprintf(stderr, "      --heightmap-range <lo> <hi>  World Y the file's range maps to\n");
    fprintf(stderr, "      --terrain-stream <p>     Stream a tiled field instead of holding one\n");
    fprintf(stderr, "      --terrain-stream-save <p>  Write the installed field as a tiled file\n");
    fprintf(stderr, "      --terrain-stream-budget <n>  Tiles read per update\n");
    fprintf(stderr, "      --terrain-stream-window <n>  Window edge in tiles\n");
    fprintf(stderr, "      --terrain-stream-resident-res <n>  Whole-level node threshold\n");
    fprintf(stderr, "      --terrain-stream-probe <n>   Print residency every N frames\n");
    fprintf(stderr, "      --terrain-height-probe   Print sampled heights, normals and masks\n");
    fprintf(stderr, "      --scatter-probe          Print the drainage the scatter placed into\n");
    fprintf(stderr, "      --cluster-probe          Print each clustered prototype's DAG\n");
    fprintf(stderr, "      --no-morph               CDLOD morph off, in all five geometry programs\n");
    fprintf(stderr, "      --terrain-quadtree-probe Print the patch selection and morph windows\n");
    fprintf(stderr, "      --terrain-extent <f>     Domain half-width; the world grows with it\n");
    fprintf(stderr, "      --no-regions             One resident region over the whole domain\n");
    fprintf(stderr, "      --region-radius <f>      Load radius; --region-span <f> the cell side\n");
    fprintf(stderr, "      --region-probe           Print residency and each region's scatter\n");
    fprintf(stderr, "      --walk <speed>           Walk the character forward, about-face at half\n");
    fprintf(stderr, "      --no-island              Flat domain and no sea, as before 11.63\n");
    fprintf(stderr, "      --no-trail               No gravel path, and no props kept off it\n");
    fprintf(stderr, "      --seed N            Terrain and scatter seed\n");
    fprintf(stderr, "      --screenshot-every N  Also save numbered frames every N frames\n");
    fprintf(stderr, "      --world-offset N    Place the whole world N units from the origin\n");
    fprintf(stderr, "      --origin-shift-at F Re-centre the world on the camera at frame F\n");
    fprintf(stderr, "      --origin-shift-distance D  Re-centre whenever the camera drifts D\n");
    fprintf(stderr, "      --cam-eye x,y,z     Pin the camera (disables follow)\n");
    fprintf(stderr, "      --cam-target x,y,z  Pinned camera aim point\n");
}

static bool parse_vec3(const char* s, vec3 out) {
    return sscanf(s, "%f,%f,%f", &out[0], &out[1], &out[2]) == 3;
}

int main(int argc, char** argv) {
    memset(&g_args, 0, sizeof(g_args));
    g_args.seed = 1337u;
    g_args.sun_elevation = -1000.0f; // sentinel: keep the app's own angle
    g_args.sun_azimuth = -1000.0f;
    int cam_eye_set = 0, cam_target_set = 0;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "-x") || !strcmp(a, "--headless")) {
            g_args.headless = 1;
        } else if ((!strcmp(a, "-f") || !strcmp(a, "--frames")) && i + 1 < argc) {
            g_args.frames = atoi(argv[++i]);
        } else if ((!strcmp(a, "-S") || !strcmp(a, "--screenshot")) && i + 1 < argc) {
            g_args.screenshot = argv[++i];
        } else if (!strcmp(a, "--screenshot-every") && i + 1 < argc) {
            g_args.screenshot_every = atoi(argv[++i]);
        } else if (!strcmp(a, "--profiler")) {
            g_args.profiler = 1;
        } else if (!strcmp(a, "--no-clusters")) {
            g_args.no_clusters = 1;
        } else if (!strcmp(a, "--no-quadtree")) {
            g_args.no_quadtree = 1;
        } else if (!strcmp(a, "--no-morph")) {
            g_args.no_morph = 1;
        } else if (!strcmp(a, "--terrain-quadtree-probe")) {
            g_args.quadtree_probe = 1;
        } else if (!strcmp(a, "--terrain-extent") && i + 1 < argc) {
            g_args.terrain_extent = (float)atof(argv[++i]);
        } else if (!strcmp(a, "--no-regions")) {
            g_args.no_regions = 1;
        } else if (!strcmp(a, "--region-probe")) {
            g_args.region_probe = 1;
        } else if (!strcmp(a, "--region-radius") && i + 1 < argc) {
            g_args.region_radius = (float)atof(argv[++i]);
        } else if (!strcmp(a, "--region-span") && i + 1 < argc) {
            g_args.region_span = (float)atof(argv[++i]);
        } else if (!strcmp(a, "--walk") && i + 1 < argc) {
            g_args.walk = (float)atof(argv[++i]);
        } else if (!strcmp(a, "--no-island")) {
            g_args.no_island = 1;
        } else if (!strcmp(a, "--no-trail")) {
            g_args.no_trail = 1;
        } else if (!strcmp(a, "--no-lod")) {
            g_args.no_lod = 1;
        } else if (!strcmp(a, "--no-instancing")) {
            g_args.no_instancing = 1;
        } else if (!strcmp(a, "--no-layers-vt")) {
            g_args.no_layers_vt = 1;
        } else if (!strcmp(a, "--layers-vt-res") && i + 1 < argc) {
            g_args.layers_vt_res = atoi(argv[++i]);
        } else if (!strcmp(a, "--no-layers-vt-pages")) {
            g_args.no_layers_vt_pages = 1;
        } else if (!strcmp(a, "--no-layers-vt-feedback")) {
            g_args.no_layers_vt_feedback = 1;
        } else if (!strcmp(a, "--layers-vt-page-slots") && i + 1 < argc) {
            g_args.layers_vt_page_slots = atoi(argv[++i]);
        } else if (!strcmp(a, "--layers-vt-page-budget") && i + 1 < argc) {
            g_args.layers_vt_page_budget = atoi(argv[++i]);
        } else if (!strcmp(a, "--layers-vt-probe") && i + 1 < argc) {
            g_args.layers_vt_probe = atoi(argv[++i]);
        } else if (!strcmp(a, "--no-sort-opaque")) {
            g_args.no_sort_opaque = 1;
        } else if (!strcmp(a, "--depth-prepass")) {
            g_args.depth_prepass = 1;
        } else if (!strcmp(a, "--taa")) {
            g_args.force_taa = 1;
        } else if (!strcmp(a, "--msaa") && i + 1 < argc) {
            g_args.msaa = atoi(argv[++i]);
        } else if (!strcmp(a, "--headless-jitter")) {
            g_args.headless_jitter = 1;
        } else if (!strcmp(a, "--lod-bias") && i + 1 < argc) {
            g_args.lod_bias = strtof(argv[++i], NULL);
        } else if ((!strcmp(a, "-W") || !strcmp(a, "--width")) && i + 1 < argc) {
            g_args.width = atoi(argv[++i]);
        } else if ((!strcmp(a, "-H") || !strcmp(a, "--height")) && i + 1 < argc) {
            g_args.height = atoi(argv[++i]);
        } else if (!strcmp(a, "--no-sky")) {
            g_args.no_sky = 1;
        } else if (!strcmp(a, "--no-fog")) {
            g_args.no_fog = 1;
        } else if (!strcmp(a, "--water")) {
            g_args.water = 1;
        } else if (!strcmp(a, "--water-level") && i + 1 < argc) {
            g_args.water_level = strtof(argv[++i], NULL);
            g_args.water = 1;
        } else if (!strcmp(a, "--erode")) {
            g_args.erode = 1;
        } else if (!strcmp(a, "--erode-res") && i + 1 < argc) {
            g_args.erode_res = atoi(argv[++i]);
            g_args.erode = 1;
        } else if (!strcmp(a, "--erode-iterations") && i + 1 < argc) {
            g_args.erode_iterations = atoi(argv[++i]);
            g_args.erode = 1;
        } else if (!strcmp(a, "--erode-workers") && i + 1 < argc) {
            g_args.erode_workers = atoi(argv[++i]);
            g_args.erode = 1;
        } else if (!strcmp(a, "--terrain-erosion-probe")) {
            g_args.erode_probe = 1;
            g_args.erode = 1;
        } else if (!strcmp(a, "--erode-save") && i + 1 < argc) {
            g_args.erode_save = argv[++i];
            g_args.erode = 1;
        } else if (!strcmp(a, "--heightmap") && i + 1 < argc) {
            g_args.heightmap = argv[++i];
        } else if (!strcmp(a, "--heightmap-range") && i + 2 < argc) {
            g_args.heightmap_min = strtof(argv[++i], NULL);
            g_args.heightmap_max = strtof(argv[++i], NULL);
        } else if (!strcmp(a, "--terrain-stream") && i + 1 < argc) {
            g_args.terrain_stream = argv[++i];
        } else if (!strcmp(a, "--terrain-stream-save") && i + 1 < argc) {
            g_args.terrain_stream_save = argv[++i];
        } else if (!strcmp(a, "--terrain-stream-budget") && i + 1 < argc) {
            g_args.terrain_stream_budget = atoi(argv[++i]);
        } else if (!strcmp(a, "--terrain-stream-window") && i + 1 < argc) {
            g_args.terrain_stream_window = atoi(argv[++i]);
        } else if (!strcmp(a, "--terrain-stream-resident-res") && i + 1 < argc) {
            g_args.terrain_stream_resident_res = atoi(argv[++i]);
        } else if (!strcmp(a, "--terrain-stream-probe") && i + 1 < argc) {
            g_args.terrain_stream_probe = atoi(argv[++i]);
        } else if (!strcmp(a, "--cluster-probe")) {
            g_args.cluster_probe = 1;
        } else if (!strcmp(a, "--scatter-probe")) {
            g_args.scatter_probe = 1;
        } else if (!strcmp(a, "--terrain-height-probe")) {
            g_args.height_probe = 1;
        } else if (!strcmp(a, "--trace-player")) {
            g_args.trace_player = 1;
        } else if (!strcmp(a, "--no-aerial")) {
            g_args.no_aerial = 1;
        } else if (!strcmp(a, "--sun-elevation") && i + 1 < argc) {
            g_args.sun_elevation = strtof(argv[++i], NULL);
        } else if (!strcmp(a, "--sun-azimuth") && i + 1 < argc) {
            g_args.sun_azimuth = strtof(argv[++i], NULL);
        } else if (!strcmp(a, "--no-spatial-sort")) {
            g_args.no_spatial_sort = 1;
        } else if (!strcmp(a, "--render-mode") && i + 1 < argc) {
            g_args.render_mode = atoi(argv[++i]);
        } else if (!strcmp(a, "--seed") && i + 1 < argc) {
            g_args.seed = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(a, "--world-offset") && i + 1 < argc) {
            g_args.world_offset = (float)atof(argv[++i]);
        } else if (!strcmp(a, "--origin-shift-at") && i + 1 < argc) {
            g_args.origin_shift_at = atoi(argv[++i]);
        } else if (!strcmp(a, "--origin-shift-distance") && i + 1 < argc) {
            g_args.origin_shift_distance = (float)atof(argv[++i]);
        } else if (!strcmp(a, "--cam-eye") && i + 1 < argc) {
            cam_eye_set = parse_vec3(argv[++i], g_args.cam_eye);
        } else if (!strcmp(a, "--cam-target") && i + 1 < argc) {
            cam_target_set = parse_vec3(argv[++i], g_args.cam_target);
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            print_usage(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }
    // Both or neither: half a camera would silently aim at the origin.
    g_args.cam_set = cam_eye_set && cam_target_set;

    GameConfig config = game_default_config();
    config.title = "Cetra Forest";
    config.width = g_args.width > 0 ? g_args.width : 1600;
    config.height = g_args.height > 0 ? g_args.height : 900;
    config.headless = g_args.headless != 0;
    config.exit_after_frames = g_args.frames;
    config.screenshot_path = g_args.screenshot;
    config.screenshot_every = g_args.screenshot_every;
    config.profiler = g_args.profiler != 0;

    Game* game = create_game(&config);
    if (!game) {
        fprintf(stderr, "forest: failed to create game\n");
        return 1;
    }

    game_set_init(game, on_init);
    game_set_update(game, on_update);
    game_set_render(game, on_render);
    game_set_shutdown(game, on_shutdown);

    game->engine->headless_jitter = g_args.headless_jitter != 0;
    // TAA replaces MSAA rather than joining it, which is what the render app has
    // always done and what this app was missing: it shipped 4x MSAA with no
    // temporal filter at all, so masked foliage got raw coverage dither and the
    // dark A2C fringe spec 11.19 measured, with nothing to resolve either.
    //
    // Headless keeps MSAA and skips TAA unless asked, because jitter plus a
    // history makes the frame sensitive to async load timing -- the same
    // diagnostic-only escape the render app carries.
    if (!g_args.headless || g_args.force_taa) {
        set_engine_msaa_samples(game->engine, 1);
        set_engine_taa_enabled(game->engine, true);
    }
    // After the policy, so --taa --msaa 4 is expressible -- the lever that
    // prices a sample (spec 11.34); nothing else varies the count with TAA held.
    if (g_args.msaa > 0)
        set_engine_msaa_samples(game->engine, g_args.msaa);

    run_game(game);
    free_game(game);
    return 0;
}
