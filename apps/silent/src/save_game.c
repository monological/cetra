#include "save_game.h"

#include <stdio.h>
#include <string.h>

#include <GLFW/glfw3.h>

#include "cetra/util.h"
#include "cetra/game/save.h"

#include "sounds.h"

// A room's own save waits this long after the last write: a doorway walked back and forth is not
// worth a write each way.
#define ROOM_SAVE_GAP 10.0

// Everything a save keeps, as one struct the tables are offsets into. Copied from the world before
// a write and pushed back into it after a read.
typedef struct SilentSave {
    double played; // seconds of play, kept here and nowhere else
    vec3 feet;
    float yaw, pitch;
    bool bag;                   // the backpack taken
    int item_order[ITEM_COUNT]; // each thing's place in the order it was had, from 1; 0 not had
    bool found[PLACE_COUNT];
    bool inked[PLACE_COUNT];
    float door_travel[SAVE_GAME_DOORS];
    float door_want[SAVE_GAME_DOORS];
    bool flashlight_on;
    double drafted; // when the basement door first let the draft in, sim seconds; -1 never
    int cat_place;
    float cat_energy, cat_boredom, cat_alarm, cat_affinity;
    float cat_since[CAT_PLACE_COUNT];
} SilentSave;

static SaveSystem* g_system;
static SaveWorld g_world;
static SilentSave g_save;
static char g_slot[16]; // the slot autosaves go to, or empty
static bool g_due;      // a save is due at the next frame
static bool g_room_due; // and one for a room, once the gap allows
static double g_since_write;
static int g_room = -1;

static const SaveField SUMMARY[] = {
    SAVE_ROW(SAVE_DOUBLE, "played", SilentSave, played),
    SAVE_ROW(SAVE_VEC3, "feet", SilentSave, feet),
};
static const SaveField PLAYER[] = {
    SAVE_ROW(SAVE_FLOAT, "yaw", SilentSave, yaw),
    SAVE_ROW(SAVE_FLOAT, "pitch", SilentSave, pitch),
};
static const SaveField HOUSE[] = {
    SAVE_ROW(SAVE_BOOL, "flashlight_on", SilentSave, flashlight_on),
    SAVE_ROW(SAVE_DOUBLE, "basement_draft", SilentSave, drafted),
};

// The tables keyed by each thing's own id, built once the ids are known: a row per item, place,
// door and cat place. The door keys are composed, so they are kept here.
static const char* g_cat_place_names[CAT_PLACE_COUNT];
static SaveField g_items[1 + ITEM_COUNT];
static SaveField g_finds[PLACE_COUNT];
static SaveField g_inked[PLACE_COUNT];
static SaveField g_doors[2 * SAVE_GAME_DOORS];
static char g_door_keys[SAVE_GAME_DOORS][80];
static SaveField g_cat[5];
static SaveField g_cat_since[CAT_PLACE_COUNT];

#define AT(member) offsetof(SilentSave, member)

static SaveField row(unsigned char type, const char* key, size_t offset) {
    return (SaveField){.type = type, .key = key, .offset = offset};
}

#define REGISTER(section, rows, count) \
    save_register_table(g_system, section, 1, rows, count, &g_save)

bool save_game_start(const SaveWorld* world) {
    g_world = *world;
    g_system = create_save_system(world->game);
    if (!g_system)
        return false;
    REGISTER("summary", SUMMARY, KIT_COUNT(SUMMARY));
    REGISTER("player", PLAYER, KIT_COUNT(PLAYER));
    REGISTER("house", HOUSE, KIT_COUNT(HOUSE));

    g_items[0] = row(SAVE_BOOL, "backpack", AT(bag));
    for (int i = 0; i < ITEM_COUNT; i++)
        g_items[1 + i] = row(SAVE_INT, ITEMS[i].id, AT(item_order) + (size_t)i * sizeof(int));
    REGISTER("items", g_items, 1 + ITEM_COUNT);

    for (int i = 0; i < PLACE_COUNT; i++) {
        g_finds[i] = row(SAVE_BOOL, PLACES[i].id, AT(found) + (size_t)i * sizeof(bool));
        g_inked[i] = row(SAVE_BOOL, PLACES[i].id, AT(inked) + (size_t)i * sizeof(bool));
    }
    REGISTER("finds", g_finds, PLACE_COUNT);
    REGISTER("inked", g_inked, PLACE_COUNT);

    const int doors = world->door_count < SAVE_GAME_DOORS ? world->door_count : SAVE_GAME_DOORS;
    for (int d = 0; d < doors; d++) {
        const char* name = world->doors[d]->entity->name;
        snprintf(g_door_keys[d], sizeof(g_door_keys[d]), "%s.want", name);
        g_doors[2 * d] = row(SAVE_FLOAT, name, AT(door_travel) + (size_t)d * sizeof(float));
        g_doors[2 * d + 1] =
            row(SAVE_FLOAT, g_door_keys[d], AT(door_want) + (size_t)d * sizeof(float));
    }
    if (doors > 0)
        REGISTER("doors", g_doors, 2 * doors);

    // The cat only with a mind to restore: held on a clip or sent somewhere from the command
    // line, it has none, and nothing about it is the game's.
    if (world->cat && world->cat->entity && world->mind && world->mind->brain) {
        for (int i = 0; i < CAT_PLACE_COUNT; i++) {
            g_cat_place_names[i] = CAT_PLACES[i].name;
            g_cat_since[i] =
                row(SAVE_FLOAT, CAT_PLACES[i].name, AT(cat_since) + (size_t)i * sizeof(float));
        }
        g_cat[0] = (SaveField)SAVE_ROW_ENUM("place", SilentSave, cat_place, g_cat_place_names,
                                            CAT_PLACE_COUNT);
        g_cat[1] = row(SAVE_FLOAT, "energy", AT(cat_energy));
        g_cat[2] = row(SAVE_FLOAT, "boredom", AT(cat_boredom));
        g_cat[3] = row(SAVE_FLOAT, "alarm", AT(cat_alarm));
        g_cat[4] = row(SAVE_FLOAT, "affinity", AT(cat_affinity));
        REGISTER("cat", g_cat, KIT_COUNT(g_cat));
        REGISTER("cat_since", g_cat_since, CAT_PLACE_COUNT);
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
    player_feet(w->player, g_save.feet);
    g_save.yaw = w->player->rig->yaw;
    g_save.pitch = w->player->rig->pitch;

    g_save.bag = w->backpack->taken;
    memset(g_save.item_order, 0, sizeof(g_save.item_order));
    for (int k = 0; k < w->backpack->held_count; k++)
        g_save.item_order[w->backpack->held[k]] = k + 1;

    memcpy(g_save.found, w->finds->found, sizeof(g_save.found));
    for (int i = 0; i < PLACE_COUNT; i++)
        g_save.inked[i] = map_screen_inked(w->map_screen, (PlaceId)i);

    for (int d = 0; d < w->door_count && d < SAVE_GAME_DOORS; d++) {
        g_save.door_travel[d] = w->doors[d]->travel;
        g_save.door_want[d] = w->doors[d]->want;
    }
    g_save.flashlight_on = w->lights->flashlight_on;
    g_save.drafted = w->basement->drafted;

    if (w->mind && w->mind->brain) {
        const int at = cat_nearest_place(w->cat);
        if (at >= 0)
            g_save.cat_place = at;
        g_save.cat_energy = w->mind->energy;
        g_save.cat_boredom = w->mind->boredom;
        g_save.cat_alarm = w->mind->alarm;
        g_save.cat_affinity = w->mind->affinity;
        memcpy(g_save.cat_since, w->mind->since, sizeof(g_save.cat_since));
    }
}

static void apply(void) {
    const SaveWorld* w = &g_world;
    // The bag first: the map off the fridge and the flashlight's light both follow from it.
    ItemId held[ITEM_COUNT] = {ITEM_NONE};
    int count = 0;
    for (int order = 1; order <= ITEM_COUNT; order++)
        for (int i = 0; i < ITEM_COUNT; i++)
            if (g_save.item_order[i] == order)
                held[count++] = (ItemId)i;
    backpack_restore(w->backpack, g_save.bag, held, count);
    if (backpack_holds(w->backpack, ITEM_TOWN_MAP))
        fridge_map_take(w->fridge_map);

    memcpy(w->finds->found, g_save.found, sizeof(g_save.found));
    map_screen_restore(w->map_screen, g_save.inked);

    for (int d = 0; d < w->door_count && d < SAVE_GAME_DOORS; d++)
        door_set(w->doors[d], g_save.door_travel[d], g_save.door_want[d]);
    w->lights->flashlight_on = g_save.flashlight_on && backpack_holds(w->backpack, ITEM_FLASHLIGHT);
    w->basement->drafted = g_save.drafted;

    if (w->mind && w->mind->brain) {
        cat_put(w->cat, g_save.cat_place);
        cat_mind_restore(w->mind, g_save.cat_energy, g_save.cat_boredom, g_save.cat_alarm,
                         g_save.cat_affinity, g_save.cat_since);
    }

    // The engine's own section has put the body; what follows from where it stands is said
    // nothing about, since the player did not arrive anywhere.
    player_restore(w->player, g_save.yaw, g_save.pitch);
    vec3 feet = {0.0f, 0.0f, 0.0f};
    player_feet(w->player, feet);
    finds_settle(w->finds, feet);
    basement_settle(w->basement, feet);
    g_room = sounds_room_at(feet);
}

// ------------------------------------------------------------------------------- the slots

static bool slot_path(const char* slot, char* out, size_t cap) {
    return save_default_path(out, cap, slot);
}

static void slot_name(int i, char* out, size_t cap) {
    snprintf(out, cap, "silent-%d", i + 1);
}

int save_game_list(SaveSlot* out) {
    int n = 0;
    for (int i = 0; g_system && i < SAVE_GAME_SLOTS; i++) {
        SaveSlot s = {0};
        slot_name(i, s.name, sizeof(s.name));
        char path[1024];
        if (!slot_path(s.name, path, sizeof(path)) || !path_exists(path))
            continue;
        SilentSave peeked = {0};
        SavePeek info = {0};
        if (!save_peek(g_system, path, "summary", &peeked, &info))
            continue;
        s.saved_at = info.saved_at;
        s.played = peeked.played;
        glm_vec3_copy(peeked.feet, s.feet);
        // Newest first, by insertion.
        int k = n++;
        for (; k > 0 && out[k - 1].saved_at < s.saved_at; k--)
            out[k] = out[k - 1];
        out[k] = s;
    }
    return n;
}

bool save_game_free_slot(SaveSlot* out) {
    for (int i = 0; i < SAVE_GAME_SLOTS; i++) {
        *out = (SaveSlot){0};
        slot_name(i, out->name, sizeof(out->name));
        char path[1024];
        if (slot_path(out->name, path, sizeof(path)) && !path_exists(path))
            return true;
    }
    return false;
}

bool save_game_load(const char* slot) {
    char path[1024];
    if (!g_system || !slot_path(slot, path, sizeof(path)))
        return false;
    // Over what the world was built with, so a section the file lacks keeps the new game's.
    gather();
    const SaveLoadResult r = save_read(g_system, path);
    if (!r.ok) {
        fprintf(stderr, "silent: the game in %s could not be read\n", slot);
        return false;
    }
    apply();
    printf("silent: loaded %s, %.0f s played\n", slot, g_save.played);
    return true;
}

bool save_game_delete(const char* slot) {
    char path[1024];
    return slot_path(slot, path, sizeof(path)) && remove(path) == 0;
}

// ------------------------------------------------------------------------------- autosaves

void save_game_begin(const char* slot) {
    snprintf(g_slot, sizeof(g_slot), "%s", slot ? slot : "");
    g_due = g_room_due = false;
    g_since_write = 0.0;
}

const char* save_game_slot(void) {
    return g_slot[0] ? g_slot : NULL;
}

static void write_now(const char* why) {
    char path[1024];
    if (!g_system || !g_slot[0] || !slot_path(g_slot, path, sizeof(path)))
        return;
    const double start = glfwGetTime();
    gather();
    const bool ok = save_write(g_system, path);
    printf("silent: %s %s (%s) in %.1f ms\n", ok ? "saved" : "could not save", g_slot, why,
           (glfwGetTime() - start) * 1000.0);
    g_due = g_room_due = false;
    g_since_write = 0.0;
}

void save_game_moment(SaveMoment moment) {
    switch (moment) {
        case SAVE_TOOK:
        case SAVE_FOUND:
            g_due = true;
            break;
        case SAVE_ROOM:
            g_room_due = true;
            break;
        case SAVE_QUIT:
            write_now("quit");
            break;
    }
}

void save_game_frame(const vec3 feet, double dt) {
    g_save.played += dt;
    g_since_write += dt;
    const int room = sounds_room_at(feet);
    if (g_room >= 0 && room != g_room)
        save_game_moment(SAVE_ROOM);
    g_room = room;
    if (g_due)
        write_now("progress");
    else if (g_room_due && g_since_write >= ROOM_SAVE_GAP)
        write_now(sounds_room_label(room));
}
