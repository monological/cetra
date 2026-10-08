#include <assert.h>
#include <stdio.h>

#include "cetra/material.h"
#include "cetra/postfx.h"
#include "cetra/program.h"
#include "cetra/texture.h"
#include "cetra/util.h"

#include "mats.h"

/*
 * The photo sets under assets/textures/silent/, as tools/fetch_textures.py
 * leaves them: CC0 scans from Poly Haven and ambientCG cut down to 256 square and stored
 * bottom row first, so V runs up a wall. Relative to the repository root,
 * which is where every app in this tree is run from.
 */
#define TEXTURE_DIR "assets/textures/silent"

/*
 * Glass: what it transmits, how thick a path through it is, and the colour
 * that path leaves. The containers' contents are opaque meshes inside them,
 * which the refraction pass sees through the shell.
 *
 * A container is HOLLOW, so its thickness is its wall's. The refraction pass
 * bends the view ray through `thickness` before it samples what is behind,
 * and a jar's diameter there sampled well past the jam standing right behind
 * the wall, so a full jar read as empty glass over a band of fill. The
 * absorption distances are the walls' too, so the tint survives the thin path.
 */
typedef struct GlassSpec {
    float transmission;   // 0 = not glass
    float thickness;      // metres
    float attenuation[3]; // what survives `distance` of glass
    float distance;
} GlassSpec;

/*
 * Emitters that are decoration and not lamps: a lit window and a street lamp's
 * lens glow, but the light they throw is authored separately (or not at all),
 * so they are kept out of the engine's derived area panels -- which would
 * otherwise try to fit a rectangle to every lit window on the street at once.
 * One glowing by its own picture takes its albedo map as its emissive map.
 */
typedef struct GlowSpec {
    float colour[3];
    float nits; // 0 = no glow
    bool own_picture;
} GlowSpec;

/*
 * What the rain does to a surface it reaches; `wet` false leaves it all to the engine.
 *
 * POROSITY, 0 sealed to 1 fully porous, is what darkens it as it wets (Material.porosity).
 * The engine derives it from roughness, rough being porous, and that is wrong for every
 * painted surface here -- a scan of weathered paint is rough and the paint still sheds water,
 * so the siding darkened like soaked concrete. Paint and slate take a little, bare wood and
 * masonry much more. -1 leaves it to the derivation.
 *
 * RELIEF stands the puddles in the scan's own lows (spec 13.12): the asphalt's dips and the
 * yard's hollows, from the displacement map the scan brings. No parallax: the map shapes the
 * puddles and nothing else.
 *
 * BEADS: glass drawn opaque -- the dark panes and the car's, the lit windows -- beads like the
 * glass that transmits, which the engine finds for itself. The jars and bottles are indoors
 * and never will, so they skip asking.
 */
typedef struct RainSpec {
    bool wet;
    float porosity;
    bool relief;
    MaterialRainBeads beads;
} RainSpec;

// Light that wanders inside a surface before it leaves it: the engine's subsurface blur, its
// strength, how far each channel goes relative to the others, and the widest's reach in metres.
typedef struct ScatterSpec {
    float strength; // 0 = none
    float colour[3];
    float radius;
} ScatterSpec;

/*
 * One surface, at its MatId's index in SPECS.
 *
 * GRIME is the dirt round its edges (spec 13.8), for the surfaces that are objects: every box
 * of these is a real thing, so every edge the kit darkens is a real edge. A wall never takes
 * it whatever its material, since the seams between the slabs it is cut into round its
 * openings are edges nobody built. On glass the dirt tints what shows through as well as the
 * surface, which is how a film of grease looks.
 */
typedef struct MatSpec {
    const char* name; // the material's own name, for the GUI's editor
    const char* set;  // the photo set's base name; NULL is a flat colour
    float tint[3];    // the albedo factor over the map
    float roughness;  // a factor over the map: under 1 is wetter
    float metallic;
    float repeat_m;    // metres one repeat covers
    bool surface_only; // take the set's normal and roughness, keep the tint as the colour
    float grime;       // 0..1
    GlassSpec glass;
    GlowSpec glow;
    RainSpec rain;
    ScatterSpec scatter;
} MatSpec;

#define STAINED_NIGHT_NITS 25.0f
#define STAINED_DAY_NITS   1500.0f

/*
 * The repeats follow each scan's real size where that reads right and depart
 * from it where the reference asks: the kitchen's 30 cm floor tiles are the
 * scan's 50 cm ones at a shorter repeat, and the backsplash is the SAME floor
 * set at 0.6 m, which makes its 15 cm glazed squares.
 */
static const MatSpec SPECS[MAT_COUNT] = {
    [MAT_PLASTER] = {"plaster", "grey_plaster_02", {0.80f, 0.90f, 0.82f}, 1.0f, 0.0f, 1.4f},
    [MAT_CEILING] = {"ceiling", "white_plaster_rough_02", {0.75f, 0.75f, 0.72f}, 1.0f, 0.0f, 1.5f},
    [MAT_KITCHEN_FLOOR] =
        {"kitchen_floor", "worn_tile_floor", {0.95f, 1.0f, 0.97f}, 0.32f, 0.0f, 1.25f},
    // Old dark boards under lacquer: the Gothic house's hardwood (spec 13.13).
    [MAT_WOOD_FLOOR] = {"wood_floor", "old_wooden_floor_02", {1, 1, 1}, 0.8f, 0.0f, 2.0f},
    [MAT_BACKSPLASH] = {"backsplash", "worn_tile_floor", {0.88f, 0.98f, 1.02f}, 0.45f, 0.0f, 0.6f},
    [MAT_ENAMEL] =
        {"enamel", "rusty_metal_02", {0.88f, 0.9f, 0.84f}, 0.8f, 0.0f, 1.0f, .grime = 0.75f},
    // The dirty-white wall scan again, yellowed and matte: old enamel that has
    // gone grey with grime rather than to rust.
    [MAT_APPLIANCE] =
        {"appliance", "concrete_wall_003", {0.82f, 0.80f, 0.70f}, 1.0f, 0.0f, 1.5f, .grime = 0.6f},
    [MAT_TRIM] = {"trim",
                  "concrete_wall_003",
                  {0.82f, 0.86f, 0.80f},
                  1.0f,
                  0.0f,
                  1.5f,
                  .grime = 0.55f,
                  .rain = {true, 0.15f}},
    [MAT_STEEL] = {"steel", "Metal009", {1, 1, 1}, 1.0f, 1.0f, 0.6f, .grime = 0.5f},
    [MAT_WOOD] = {"wood",
                  "wood_table_worn",
                  {1, 1, 1},
                  1.0f,
                  0.0f,
                  0.8f,
                  .grime = 0.45f,
                  .rain = {true, 0.2f}},
    [MAT_SIDING] = {"siding",
                    "white_planks_clean",
                    {0.78f, 0.84f, 0.82f},
                    1.0f,
                    0.0f,
                    1.8f,
                    .rain = {true, 0.25f}},
    [MAT_SIDING_B] =
        {"siding_blue", "blue_painted_planks", {1, 1, 1}, 1.0f, 0.0f, 1.2f, .rain = {true, 0.25f}},
    [MAT_SIDING_C] = {"siding_ochre",
                      "white_planks_clean",
                      {0.86f, 0.76f, 0.56f},
                      1.0f,
                      0.0f,
                      1.8f,
                      .rain = {true, 0.25f}},
    [MAT_PORCH] =
        {"porch", "old_wood_floor", {0.75f, 0.78f, 0.8f}, 1.0f, 0.0f, 2.0f, .rain = {true, 0.7f}},
    [MAT_DIRT] = {"yard", "grass_ground", {1, 1, 1}, 1.0f, 0.0f, 2.5f, .rain = {true, 1.0f, true}},
    [MAT_ASPHALT] =
        {"asphalt", "asphalt_02", {1, 1, 1}, 1.0f, 0.0f, 3.0f, .rain = {true, 0.9f, true}},
    [MAT_CONCRETE] =
        {"sidewalk", "concrete_pavement", {1, 1, 1}, 1.0f, 0.0f, 1.8f, .rain = {true, 0.7f, true}},
    [MAT_ROOF] = {"roof", "roof_slates_02", {1, 1, 1}, 1.0f, 0.0f, 3.0f, .rain = {true, 0.25f}},
    [MAT_BRICK] = {"brick", "brick_wall_006", {1, 1, 1}, 1.0f, 0.0f, 3.0f, .rain = {true, 0.6f}},
    [MAT_RUG] = {"rug", "dirty_carpet", {1, 1, 1}, 1.0f, 0.0f, 0.6f},
    [MAT_TOWEL] = {"towel", "fabric_pattern_05", {1, 1, 1}, 1.0f, 0.0f, 0.5f, .grime = 0.4f},
    [MAT_PAPER] = {"paper", "Paper003", {0.86f, 0.80f, 0.64f}, 1.0f, 0.0f, 0.4f, .grime = 0.35f},
    // Glass takes the kitchen smear's roughness and relief, so it is smudged
    // rather than perfect, and its colour from the tint.
    [MAT_GLASS_AMBER] = {"amber_glass",
                         "Smear008",
                         {0.90f, 0.62f, 0.36f},
                         0.12f,
                         0.0f,
                         0.3f,
                         true,
                         .grime = 0.35f,
                         .glass = {0.95f, 0.004f, {0.80f, 0.50f, 0.22f}, 0.008f},
                         .rain = {true, -1.0f, .beads = RAIN_BEADS_OFF}},
    [MAT_GLASS_CLEAR] = {"clear_glass",
                         "Smear008",
                         {0.94f, 1.0f, 0.95f},
                         0.12f,
                         0.0f,
                         0.3f,
                         true,
                         .grime = 0.35f,
                         .glass = {0.97f, 0.004f, {0.90f, 0.97f, 0.90f}, 0.025f},
                         .rain = {true, -1.0f, .beads = RAIN_BEADS_OFF}},
    // What is in the jars, off one granular scan: a red-brown sauce or spice,
    // pale grain, and something pickled.
    [MAT_CONTENTS] = {"contents_red", "grass_ground", {0.46f, 0.15f, 0.07f}, 0.5f, 0.0f, 0.25f},
    [MAT_CONTENTS_PALE] =
        {"contents_pale", "grass_ground", {1.0f, 0.86f, 0.62f}, 0.8f, 0.0f, 0.15f},
    [MAT_CONTENTS_GREEN] =
        {"contents_green", "grass_ground", {0.42f, 0.50f, 0.20f}, 0.4f, 0.0f, 0.3f},
    // Glazed and stained: the dirty-white wall scan at a small repeat, glossy.
    [MAT_CERAMIC] =
        {"ceramic", "concrete_wall_003", {1.0f, 1.0f, 0.97f}, 0.3f, 0.0f, 0.5f, .grime = 0.4f},
    [MAT_BLACK] = {"black_enamel", NULL, {0.03f, 0.03f, 0.03f}, 0.35f, 0.0f, 1.0f},
    [MAT_TABLE] =
        {"table_enamel", "PaintedMetal001", {0.50f, 0.58f, 0.66f}, 0.7f, 0.0f, 1.0f, .grime = 0.5f},
    // A window pane is THIN glass: no volume, so no bend and no absorption, only the tint and
    // the smear.
    [MAT_WINDOW_GLASS] = {"window_glass",
                          "Smear008",
                          {0.86f, 0.92f, 0.88f},
                          0.25f,
                          0.0f,
                          0.6f,
                          true,
                          .glass = {0.9f, 0.0f, {1.0f, 1.0f, 1.0f}, 0.0f}},
    [MAT_DARK_GLASS] = {"dark_glass",
                        "Smear008",
                        {0.02f, 0.025f, 0.03f},
                        0.15f,
                        0.0f,
                        0.6f,
                        true,
                        .rain = {true, -1.0f, .beads = RAIN_BEADS_ON}},
    [MAT_WINDOW_LIT] = {"window_lit",
                        "fabric_pattern_05",
                        {0.9f, 0.85f, 0.7f},
                        1.0f,
                        0.0f,
                        0.5f,
                        .glow = {{1.0f, 0.70f, 0.40f}, 30.0f},
                        .rain = {true, 0.0f, .beads = RAIN_BEADS_ON}},
    [MAT_LAMP_GLOW] = {"lamp_glow",
                       NULL,
                       {0.9f, 0.95f, 0.9f},
                       0.5f,
                       0.0f,
                       1.0f,
                       .glow = {{0.85f, 1.0f, 0.90f}, 4000.0f}},
    [MAT_LAMP_POST] = {"lamp_post", "metal_plate_02", {0.7f, 0.72f, 0.7f}, 1.0f, 0.6f, 1.0f},
    [MAT_POLE] = {"utility_pole",
                  "old_wood_floor",
                  {0.55f, 0.52f, 0.5f},
                  1.0f,
                  0.0f,
                  1.5f,
                  .rain = {true, 0.4f}},
    [MAT_CAR] = {"car_paint",
                 "rusty_metal_02",
                 {0.45f, 0.14f, 0.11f},
                 0.6f,
                 0.0f,
                 1.2f,
                 .rain = {true, 0.0f}},
    [MAT_CARDBOARD] = {"cardboard", "Cardboard003", {0.85f, 0.80f, 0.70f}, 1.0f, 0.0f, 0.6f},
    // A washed-out green, glossy: the one saturated thing by the sink.
    [MAT_PLASTIC] = {"plastic", NULL, {0.30f, 0.42f, 0.26f}, 0.35f, 0.0f, 1.0f},
    // Steel that is handled and never scrubbed: the smear scan's roughness and
    // relief, so it shines between dull smears and water spots. The colour is
    // what polished stainless reflects at normal incidence, about 0.6.
    [MAT_STAINLESS] =
        {"stainless", "Smear008", {0.6f, 0.6f, 0.58f}, 0.6f, 1.0f, 0.3f, true, .grime = 0.5f},
    // Preserves: flat and glossy, since what shows through the glass is the
    // colour and the shine off the top. Dark, but not so dark that the glass's
    // own reflection hides them: under about 0.06 a full jar read as empty.
    [MAT_JAM] = {"jam", NULL, {0.24f, 0.02f, 0.025f}, 0.3f, 0.0f, 1.0f},
    [MAT_PLUM] = {"plum", NULL, {0.13f, 0.02f, 0.085f}, 0.3f, 0.0f, 1.0f},
    [MAT_PRUNE] = {"prune", NULL, {0.10f, 0.045f, 0.02f}, 0.35f, 0.0f, 1.0f},
    // tools/make_cards.py's picture; the repeat is unused, since a card takes
    // its UVs from cards.h rather than from the world.
    [MAT_CARDS] = {"cards", "cards", {1, 1, 1}, 1.0f, 0.0f, 1.0f},
    [MAT_CUSHION] =
        {"cushion", "Sponge002", {0.95f, 0.9f, 0.78f}, 1.0f, 0.0f, 0.25f, .grime = 0.5f},
    // The clock. Veneer under old varnish: rougher than the scans' fresh
    // lacquer, which would mirror the hall. Rosewood's scan covers 2.4 m, so a
    // shorter repeat brings its flame figure down to a panel's size.
    [MAT_CASE] =
        {"clock_case", "lacquered_cherry_wood", {1, 1, 1}, 1.4f, 0.0f, 0.8f, .grime = 0.45f},
    [MAT_ROSEWOOD] = {"rosewood", "rosewood_veneer1", {1, 1, 1}, 1.4f, 0.0f, 1.0f, .grime = 0.4f},
    [MAT_MAPLE] = {"maple", "white_maple_veneer", {1, 1, 1}, 1.2f, 0.0f, 0.5f, .grime = 0.3f},
    // Brass gone dull: the smear scan's roughness and relief under brass's
    // reflectance, darkened by tarnish.
    [MAT_BRASS] =
        {"brass", "Smear008", {0.72f, 0.58f, 0.3f}, 0.8f, 1.0f, 0.3f, true, .grime = 0.5f},
    // A 25 W filament through frosted glass; its light is the point light lights.c hangs
    // inside it.
    [MAT_BULB] = {"bulb",
                  NULL,
                  {0.95f, 0.9f, 0.8f},
                  0.2f,
                  0.0f,
                  1.0f,
                  .glow = {{1.0f, 0.72f, 0.42f}, 1500.0f}},
    // The Gothic house (spec 13.13), after a weathered charcoal Carpenter Gothic in fog: the
    // boards laid upright, the battens the same paint, slates cut to fish scales. The paint is
    // lifted from the scan's near-black -- about 0.012 linear -- to a weathered charcoal near
    // 0.05, or every carving on the house is a silhouette in the fog.
    [MAT_SIDING_DARK] = {"siding_dark",
                         "black_painted_planks",
                         {4.0f, 4.0f, 4.0f},
                         1.0f,
                         0.0f,
                         1.6f,
                         .rain = {true, 0.25f}},
    [MAT_SLATE] = {"slate", "RoofingTiles002", {1, 1, 1}, 1.0f, 0.0f, 1.5f, .rain = {true, 0.25f}},
    [MAT_STONE] = {"castle_stone",
                   "medieval_blocks_03",
                   {1, 1, 1},
                   1.0f,
                   0.0f,
                   2.0f,
                   .grime = 0.45f,
                   .rain = {true, 0.6f}},
    [MAT_FOUNDATION] =
        {"foundation", "castle_wall_varriation", {1, 1, 1}, 1.0f, 0.0f, 2.0f, .rain = {true, 0.7f}},
    // Wrought iron under black paint: the painted-metal scan's wear, taken dark.
    [MAT_IRON] = {"iron",
                  "PaintedMetal001",
                  {0.07f, 0.07f, 0.07f},
                  0.6f,
                  0.0f,
                  0.5f,
                  .grime = 0.3f,
                  .rain = {true, 0.1f}},
    [MAT_LEATHER] = {"leather", "brown_leather", {1, 1, 1}, 0.7f, 0.0f, 0.4f},
    // The clock case's lacquered cherry taken to the carved panels' red-brown, which
    // make_gothic.py tints the same scan to, so the frames and the carving are one wood.
    [MAT_MAHOGANY] = {"mahogany", "lacquered_cherry_wood", {1.1f, 0.61f, 0.40f}, 1.4f, 0.0f, 0.8f},
    // tools/make_gothic.py's picture, each card placed whole by gothic.h's UVs; the repeat is
    // unused.
    [MAT_GOTHIC] = {"gothic_pictures", "gothic", {1, 1, 1}, 1.0f, 0.0f, 1.0f},
    /*
     * The study's stained glass, lit through by its own picture: by night faintly, as though
     * the moon and the street were behind it, and by day (mats_daytime) as the overcast sky
     * through it. It is OPAQUE, not a transmissive pane: the late pass writes no depth for the
     * fog, so a pane took the fog of the whole lamp-lit street behind it and washed to grey
     * from inside; and a pane only colours what comes through it, which at night is nothing.
     */
    [MAT_STAINED] = {"stained_glass",
                     "gothic",
                     {1, 1, 1},
                     1.0f,
                     0.0f,
                     1.0f,
                     .glow = {{1.0f, 1.0f, 1.0f}, STAINED_NIGHT_NITS, true},
                     .rain = {true, -1.0f, .beads = RAIN_BEADS_ON}},
    // The great hall's leaded quarries, thin glass too: the lead is a dark albedo the light
    // through them is multiplied by.
    [MAT_LEADED] = {"leaded_glass",
                    "leaded_glass",
                    {1, 1, 1},
                    1.0f,
                    0.0f,
                    0.4f,
                    .glass = {0.9f, 0.0f, {1.0f, 1.0f, 1.0f}, 0.0f}},
    // The castle stone blackened: its scan's mean, about (0.19, 0.15, 0.10) linear, taken to
    // soot's 0.02 and its warmth taken out.
    [MAT_SOOT] = {"soot", "medieval_blocks_03", {0.1f, 0.12f, 0.17f}, 1.0f, 0.0f, 2.0f},
    // Old candles gone to ivory; a banker's lamp's shade, green over white glass and glossy,
    // lit faintly by the bulb inside -- the lamp's light is the spot study.c hangs under it.
    // Wax carries light a few millimetres under its surface, red furthest, which is what
    // softens a candle's lit top into its sides; chosen by eye, there being no measured profile
    // for paraffin to hand.
    [MAT_WAX] = {"wax",
                 NULL,
                 {0.80f, 0.74f, 0.58f},
                 0.45f,
                 0.0f,
                 1.0f,
                 .scatter = {0.8f, {1.0f, 0.7f, 0.4f}, 0.006f}},
    [MAT_SHADE] = {"lamp_shade",
                   NULL,
                   {0.03f, 0.16f, 0.07f},
                   0.15f,
                   0.0f,
                   1.0f,
                   .glow = {{0.3f, 1.0f, 0.45f}, 8.0f}},
    // Brass somebody still polishes, for what holds a candle: polished brass reflects about
    // (0.91, 0.78, 0.42) linear at normal incidence, taken down a little for age, under the
    // smear scan's relief at about half the tarnished brass's roughness, and lightly grimed.
    [MAT_BRASS_BRIGHT] =
        {"brass_bright", "Smear008", {0.84f, 0.69f, 0.34f}, 0.45f, 1.0f, 0.3f, true, .grime = 0.3f},
    /*
     * The player's house (spec 13.25), after the hall in P.T.: cream walls, white mouldings, and
     * dark boards lacquered glossy enough to carry the lamps' reflections down the corridor.
     * Every one is a scan already fetched for something else, tinted.
     */
    [MAT_CREAM] = {"cream_plaster", "concrete_wall_003", {1.0f, 0.9f, 0.7f}, 1.0f, 0.0f, 2.0f},
    [MAT_HARDWOOD] = {"hardwood", "old_wooden_floor_02", {0.5f, 0.36f, 0.28f}, 0.3f, 0.0f, 2.0f},
    // A plain paper gone the colour of weak tea; a patterned scan read as a bathroom's tile.
    [MAT_WALLPAPER] = {"wallpaper", "Paper003", {0.7f, 0.58f, 0.46f}, 1.0f, 0.0f, 0.9f},
    [MAT_MOULDING] =
        {"moulding", "concrete_wall_003", {0.96f, 0.92f, 0.82f}, 0.55f, 0.0f, 1.5f, .grime = 0.4f},
    [MAT_UPHOLSTERY] =
        {"upholstery", "dirty_carpet", {0.62f, 0.44f, 0.36f}, 1.0f, 0.0f, 0.8f, .grime = 0.4f},
    [MAT_MIRROR] =
        {"mirror", "Smear008", {0.85f, 0.86f, 0.84f}, 0.06f, 1.0f, 0.4f, true, .grime = 0.6f},
    [MAT_LAMPSHADE] = {"lampshade",
                       "fabric_pattern_05",
                       {0.9f, 0.82f, 0.62f},
                       1.0f,
                       0.0f,
                       0.4f,
                       .glow = {{1.0f, 0.72f, 0.42f}, 25.0f}},
    [MAT_FROSTED] = {"frosted_glass",
                     NULL,
                     {0.95f, 0.92f, 0.85f},
                     0.3f,
                     0.0f,
                     1.0f,
                     .glow = {{1.0f, 0.74f, 0.46f}, 90.0f}},
    // A picture tube's glossy dark glass, smeared; it glows with whatever the set shows.
    [MAT_SCREEN] = {"tv_screen", "Smear008", {0.05f, 0.06f, 0.07f}, 0.15f, 0.0f, 0.3f, true},
    // The picture on the glass (spec 13.30): no photo set and no colour of its own, since the
    // late program that draws it takes nothing from the material but its params.
    [MAT_TV_STATIC] = {"tv_static", NULL, {0.0f, 0.0f, 0.0f}, 1.0f, 0.0f, 1.0f},
    /*
     * The basement (spec 13.31). The stairwell down is painted two tones, an institution's: a
     * pale paint gone the colour of old teeth, and a dark green enamel band, glossier, along the
     * flight -- both on the dirty-white wall scan, so the stains show through. At the foot that
     * stops: the basement itself is bare poured concrete, the lifts of its formwork showing,
     * darker and wet where the damp has climbed it, round a stained slab. The joists are the
     * porch's old boards gone brown.
     */
    [MAT_CELLAR_WALL] = {"cellar_wall",
                         "concrete_wall_003",
                         {0.80f, 0.82f, 0.70f},
                         1.0f,
                         0.0f,
                         1.5f,
                         .grime = 0.85f},
    [MAT_CELLAR_DADO] = {"cellar_dado",
                         "concrete_wall_003",
                         {0.13f, 0.20f, 0.16f},
                         0.45f,
                         0.0f,
                         1.5f,
                         .grime = 0.6f},
    [MAT_CELLAR_CONCRETE] = {"cellar_concrete",
                             "concrete_layers_02",
                             {0.58f, 0.60f, 0.54f},
                             1.0f,
                             0.0f,
                             2.0f,
                             .grime = 0.9f},
    [MAT_CELLAR_DAMP] = {"cellar_damp",
                         "concrete_layers_02",
                         {0.24f, 0.26f, 0.22f},
                         0.5f,
                         0.0f,
                         2.0f,
                         .grime = 0.9f},
    [MAT_CELLAR_FLOOR] =
        {"cellar_floor", "garage_floor", {0.6f, 0.6f, 0.56f}, 0.9f, 0.0f, 1.9f, .grime = 0.6f},
    [MAT_CELLAR_WET] =
        {"cellar_wet", "concrete_pavement", {0.28f, 0.29f, 0.27f}, 0.12f, 0.0f, 1.8f},
    [MAT_JOIST] =
        {"joist", "old_wood_floor", {0.62f, 0.56f, 0.48f}, 1.0f, 0.0f, 1.5f, .grime = 0.6f},
    // The cellar's own pour, out in the weather: paler, its layers the lifts it was cast in.
    [MAT_RETAINING] = {"retaining_wall",
                       "concrete_layers_02",
                       {0.66f, 0.66f, 0.62f},
                       1.0f,
                       0.0f,
                       2.4f,
                       .grime = 0.5f,
                       .rain = {true, 0.6f}},
};

static Texture* load(TexturePool* pool, const char* set, const char* map, TextureDesc desc) {
    char file[128];
    snprintf(file, sizeof(file), "%s_%s.png", set, map);
    return texture_load_file(pool, file, desc);
}

_Static_assert(MAT_COUNT <= KIT_MAX_MATERIALS, "every MatId needs a kit slot");

void mats_register(Kit* kit, Engine* engine, Scene* scene) {
    // Registered in order into an empty kit, so the slot IS the MatId.
    assert(kit->material_count == 0);
    ShaderProgram* pbr = engine_get_program(engine, CETRA_PROGRAM_PBR);
    set_texture_pool_directory(scene->tex_pool, TEXTURE_DIR);
    const TextureDesc normal_desc = {
        .is_srgb = false, .alpha = TEXTURE_ALPHA_DATA, .use = TEXTURE_USE_NORMAL};
    for (int i = 0; i < MAT_COUNT; i++) {
        const MatSpec* s = &SPECS[i];
        Material* m = create_material();
        m->name = safe_strdup(s->name);
        glm_vec3_copy((float*)s->tint, m->albedo);
        m->roughness = s->roughness;
        m->metallic = s->metallic;
        material_set_program(m, pbr);
        // The pool caches by path, so a set two materials share loads once.
        if (s->set) {
            if (!s->surface_only)
                material_set_albedo_tex(
                    m, load(scene->tex_pool, s->set, "albedo", texture_desc(true)));
            material_set_normal_tex(m, load(scene->tex_pool, s->set, "normal", normal_desc));
            material_set_roughness_tex(m,
                                       load(scene->tex_pool, s->set, "rough", texture_desc(false)));
        }
        if (s->glass.transmission > 0.0f) {
            m->transmission = s->glass.transmission;
            m->ior = 1.5f;
            m->thickness = s->glass.thickness;
            glm_vec3_copy((float*)s->glass.attenuation, m->attenuation_color);
            m->attenuation_distance = s->glass.distance;
        }
        if (s->glow.nits > 0.0f) {
            glm_vec3_copy((float*)s->glow.colour, m->emissive);
            m->emissive_strength = s->glow.nits;
            m->emissive_light = 1; // decoration: never a derived panel
            if (s->glow.own_picture)
                material_set_emissive_tex(m, m->albedo_tex);
        }
        if (s->rain.wet) {
            m->porosity = s->rain.porosity;
            m->rain_beads = s->rain.beads;
            if (s->rain.relief)
                material_set_height_tex(m,
                                        load(scene->tex_pool, s->set, "disp", texture_desc(false)));
        }
        if (s->scatter.strength > 0.0f && engine->postfx) {
            m->subsurface = s->scatter.strength;
            glm_vec3_copy((float*)s->scatter.colour, m->subsurface_color);
            m->subsurface_profile =
                postfx_add_sss_profile(engine->postfx, s->scatter.colour, s->scatter.radius);
        }
        kit_material(kit, m, s->repeat_m, s->grime);
    }
}

void mats_daytime(Kit* kit) {
    kit->materials[MAT_LAMP_GLOW]->emissive_strength = 0.0f;
    kit->materials[MAT_STAINED]->emissive_strength = STAINED_DAY_NITS;
}
