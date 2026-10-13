#include "save_game.h"

#include <stdio.h>
#include <string.h>

#include <GLFW/glfw3.h>

#include "cetra/util.h"
#include "cetra/game/save.h"

#include "rooms.h"

// A room's own save waits this long after the last write: a doorway walked back and forth is not
// worth a write each way.
#define ROOM_SAVE_GAP 10.0

// The world as it was built (spec 13.50), beside the slots and outside the ones a list shows.
#define START_SLOT "silent-start"

// What a save keeps that no module holds as a field: worked out from the world before a write and
// put back into it after a read. The summary is at offsets, which a list of saves peeks into.
typedef struct Kept {
    double played; // seconds of play, kept here and nowhere else
    vec3 feet;
    bool bag;                   // the backpack taken
    int item_order[ITEM_COUNT]; // each thing's place in the order it was had, from 1; 0 not had
    bool inked[PLACE_COUNT];
    int cat_place;
} Kept;

static SaveSystem* g_system;
static SaveWorld g_world;
static Kept g_kept;
static char g_slot[16];   // the slot autosaves go to, or empty
static char g_path[1024]; // and its file
static bool g_due;        // a save is due at the next frame
static bool g_room_due;   // and one for a room, once the gap allows
static double g_since_write;
static int g_room = -1;

static const SaveField SUMMARY[] = {
    SAVE_ROW(SAVE_DOUBLE, "played", Kept, played),
    SAVE_ROW(SAVE_VEC3, "feet", Kept, feet),
};

// Every other table is by address, keyed by each thing's own id, and built once the world is
// known.
static const char* g_cat_place_names[CAT_PLACE_COUNT];
static SaveField g_player[2];
static SaveField g_house[2];
static SaveField g_items[1 + ITEM_COUNT];
static SaveField g_finds[PLACE_COUNT];
static SaveField g_inked[PLACE_COUNT];
static SaveField g_door_travel[SAVE_GAME_DOORS];
static SaveField g_door_want[SAVE_GAME_DOORS];
static SaveField g_cat[5];
static SaveField g_cat_since[CAT_PLACE_COUNT];

#define ROW(type, key, addr) ((SaveField)SAVE_ROW_AT(type, key, addr))

// A table of rows by address, which never read their base; the save system asks for one all the
// same.
static void add_table(const char* section, const SaveField* rows, int count) {
    save_register_table(g_system, section, 1, rows, count, g_world.game);
}

// The cat only with a mind to restore: held on a clip or sent somewhere from the command line, it
// has none, and nothing about it is the game's.
static bool cat_kept(void) {
    const SaveWorld* w = &g_world;
    return w->cat && w->cat->entity && w->mind && w->mind->brain;
}

bool save_game_start(const SaveWorld* world) {
    g_world = *world;
    const SaveWorld* w = &g_world;
    g_system = create_save_system(w->game);
    if (!g_system)
        return false;
    save_register_table(g_system, "summary", 1, SUMMARY, KIT_COUNT(SUMMARY), &g_kept);

    g_player[0] = ROW(SAVE_FLOAT, "yaw", &w->player->rig->yaw);
    g_player[1] = ROW(SAVE_FLOAT, "pitch", &w->player->rig->pitch);
    add_table("player", g_player, KIT_COUNT(g_player));
    g_house[0] = ROW(SAVE_BOOL, "flashlight_on", &w->lights->flashlight_on);
    g_house[1] = ROW(SAVE_DOUBLE, "basement_draft", &w->basement->drafted);
    add_table("house", g_house, KIT_COUNT(g_house));

    g_items[0] = ROW(SAVE_BOOL, "backpack", &g_kept.bag);
    for (int i = 0; i < ITEM_COUNT; i++)
        g_items[1 + i] = ROW(SAVE_INT, ITEMS[i].id, &g_kept.item_order[i]);
    add_table("items", g_items, KIT_COUNT(g_items));

    for (int i = 0; i < PLACE_COUNT; i++) {
        g_finds[i] = ROW(SAVE_BOOL, PLACES[i].id, &w->finds->found[i]);
        g_inked[i] = ROW(SAVE_BOOL, PLACES[i].id, &g_kept.inked[i]);
    }
    add_table("finds", g_finds, PLACE_COUNT);
    add_table("inked", g_inked, PLACE_COUNT);

    for (int d = 0; d < w->door_count; d++) {
        Door* door = w->doors[d];
        g_door_travel[d] = ROW(SAVE_FLOAT, door->entity->name, &door->travel);
        g_door_want[d] = ROW(SAVE_FLOAT, door->entity->name, &door->want);
    }
    if (w->door_count > 0) {
        add_table("door_travel", g_door_travel, w->door_count);
        add_table("door_want", g_door_want, w->door_count);
    }

    if (cat_kept()) {
        CatMind* m = w->mind;
        for (int i = 0; i < CAT_PLACE_COUNT; i++) {
            g_cat_place_names[i] = CAT_PLACES[i].name;
            g_cat_since[i] = ROW(SAVE_FLOAT, CAT_PLACES[i].name, &m->since[i]);
        }
        g_cat[0] = (SaveField){.type = SAVE_ENUM,
                               .key = "place",
                               .addr = &g_kept.cat_place,
                               .names = g_cat_place_names,
                               .name_count = CAT_PLACE_COUNT};
        g_cat[1] = ROW(SAVE_FLOAT, "energy", &m->energy);
        g_cat[2] = ROW(SAVE_FLOAT, "boredom", &m->boredom);
        g_cat[3] = ROW(SAVE_FLOAT, "alarm", &m->alarm);
        g_cat[4] = ROW(SAVE_FLOAT, "affinity", &m->affinity);
        add_table("cat", g_cat, KIT_COUNT(g_cat));
        add_table("cat_since", g_cat_since, CAT_PLACE_COUNT);
    }
    return true;
}

void save_game_free(void) {
    free_save_system(g_system);
    g_system = NULL;
}

// ------------------------------------------------------------------------------- the world

static void gather(void) {
    const SaveWorld* w = &g_world;
    player_feet(w->player, g_kept.feet);
    g_kept.bag = w->backpack->taken;
    memset(g_kept.item_order, 0, sizeof(g_kept.item_order));
    for (int k = 0; k < w->backpack->held_count; k++)
        g_kept.item_order[w->backpack->held[k]] = k + 1;
    for (int i = 0; i < PLACE_COUNT; i++)
        g_kept.inked[i] = map_screen_inked(w->map_screen, (PlaceId)i);
    if (cat_kept())
        g_kept.cat_place = cat_nearest_place(w->cat);
}

static void apply(void) {
    const SaveWorld* w = &g_world;
    // The bag first: the map off the fridge and the flashlight's light both follow from it.
    ItemId held[ITEM_COUNT] = {ITEM_NONE};
    int count = 0;
    for (int order = 1; order <= ITEM_COUNT; order++)
        for (int i = 0; i < ITEM_COUNT; i++)
            if (g_kept.item_order[i] == order)
                held[count++] = (ItemId)i;
    backpack_restore(w->backpack, g_kept.bag, held, count);
    fridge_map_on_door(w->fridge_map, !backpack_holds(w->backpack, ITEM_TOWN_MAP));
    w->lights->flashlight_on =
        w->lights->flashlight_on && backpack_holds(w->backpack, ITEM_FLASHLIGHT);
    map_screen_restore(w->map_screen, g_kept.inked);

    for (int d = 0; d < w->door_count; d++)
        door_set(w->doors[d], w->doors[d]->travel, w->doors[d]->want);
    if (cat_kept()) {
        cat_put(w->cat, g_kept.cat_place);
        cat_mind_restore(w->mind);
    }

    // The engine's own section has put the body and the clock; what follows from where it stands
    // is said nothing about, since the player did not arrive anywhere.
    player_restore(w->player);
    vec3 feet = {0.0f, 0.0f, 0.0f};
    player_feet(w->player, feet);
    finds_settle(w->finds, feet);
    basement_settle(w->basement, feet, w->game->time);
    g_room = (int)room_at(feet);
}

// ------------------------------------------------------------------------------- the slots

static void slot_name(int i, char* out, size_t cap) {
    snprintf(out, cap, "silent-%d", i + 1);
}

int save_game_list(SaveSlot* out) {
    int n = 0;
    for (int i = 0; g_system && i < SAVE_GAME_SLOTS; i++) {
        SaveSlot s = {0};
        slot_name(i, s.name, sizeof(s.name));
        char path[1024];
        if (!save_default_path(path, sizeof(path), s.name) || !path_exists(path))
            continue;
        Kept peeked = {0};
        SavePeek info = {0};
        if (!save_peek(g_system, path, "summary", &peeked, &info))
            continue;
        s.saved_at = info.saved_at;
        s.played = peeked.played;
        s.where = room_label(room_at(peeked.feet));
        // Newest first, by insertion.
        int k = n++;
        for (; k > 0 && out[k - 1].saved_at < s.saved_at; k--)
            out[k] = out[k - 1];
        out[k] = s;
    }
    return n;
}

bool save_game_unused_slot(SaveSlot* out) {
    for (int i = 0; i < SAVE_GAME_SLOTS; i++) {
        *out = (SaveSlot){0};
        slot_name(i, out->name, sizeof(out->name));
        char path[1024];
        if (save_default_path(path, sizeof(path), out->name) && !path_exists(path))
            return true;
    }
    return false;
}

bool save_game_load(const char* slot) {
    char path[1024];
    if (!g_system || !save_default_path(path, sizeof(path), slot))
        return false;
    // Over what the world was built with, so a section the file lacks keeps the new game's, and
    // over a cat's mind as it was made, which a file holds only the needs of.
    gather();
    if (cat_kept())
        cat_mind_renew(g_world.mind);
    const SaveLoadResult r = save_read(g_system, path);
    if (!r.ok) {
        fprintf(stderr, "silent: the game in %s could not be read\n", slot);
        return false;
    }
    apply();
    printf("silent: loaded %s, %.0f s played\n", slot, g_kept.played);
    return true;
}

bool save_game_delete(const char* slot) {
    char path[1024];
    return save_default_path(path, sizeof(path), slot) && remove(path) == 0;
}

// The world as it is now into `path`.
static bool write_to(const char* path) {
    gather();
    return save_write(g_system, path);
}

bool save_game_keep_start(void) {
    char path[1024];
    if (!g_system || !save_default_path(path, sizeof(path), START_SLOT))
        return false;
    if (write_to(path))
        return true;
    fprintf(stderr, "silent: the world's start could not be kept\n");
    return false;
}

bool save_game_load_start(void) {
    return save_game_load(START_SLOT);
}

// ------------------------------------------------------------------------------- autosaves

void save_game_begin(const char* slot) {
    g_slot[0] = g_path[0] = '\0';
    if (slot && save_default_path(g_path, sizeof(g_path), slot))
        snprintf(g_slot, sizeof(g_slot), "%s", slot);
    else if (slot)
        fprintf(stderr, "silent: nowhere to keep %s; this game is not saved\n", slot);
    g_due = g_room_due = false;
    g_since_write = 0.0;
}

static bool write_now(const char* why) {
    if (!g_system || !g_slot[0])
        return false;
    const double start = glfwGetTime();
    const bool ok = write_to(g_path);
    printf("silent: %s %s (%s) in %.1f ms\n", ok ? "saved" : "could not save", g_slot, why,
           (glfwGetTime() - start) * 1000.0);
    g_due = g_room_due = false;
    g_since_write = 0.0;
    return ok;
}

void save_game_due(void) {
    g_due = true;
}

void save_game_quit(void) {
    write_now("quit");
}

bool save_game_frame(const vec3 feet, double dt) {
    g_kept.played += dt;
    g_since_write += dt;
    const RoomId room = room_at(feet);
    if (g_room >= 0 && (int)room != g_room)
        g_room_due = true;
    g_room = (int)room;
    if (g_due)
        return write_now("progress");
    if (g_room_due && g_since_write >= ROOM_SAVE_GAP)
        return write_now(room_label(room));
    return false;
}
