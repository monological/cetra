#include "render_tags.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cetra/mesh.h"
#include "cetra/scene.h"

void render_tags_usage(void) {
    fprintf(stderr, "      --node-tag [F:]NAME=TAG[:CLASS]  The named node's tag and class\n");
    fprintf(stderr, "      --piece-tags NAME=FIRST  Each separate piece of the node's first mesh "
                    "a tag of its own, from FIRST\n");
    fprintf(stderr, "      --tag [F:]TAG=SPEC  A row of the views' table: none, or colour=RRGGBBAA,"
                    "outline=RRGGBBAA,dim=D,hidden,lit\n");
    fprintf(stderr, "      --tag-class [F:][V/]CLASS=SPEC  A class row, every view's or grid view "
                    "V's: none, or colour=RRGGBBAA,see=S,flat=F,hidden\n");
    fprintf(stderr, "      --tag-fade [F:][V/]S  How far what is not lit fades, 0..1\n");
    fprintf(stderr, "      --tag-table  The views borrow a table with nothing written\n");
    fprintf(stderr, "      --tag-views MASK  The grid views that borrow the table, a bit each\n");
}

// An optional "F:" before the rest, and an optional "V/" after it when `view` is asked for.
static const char* _prefix(const char* spec, int* frame, int* view) {
    *frame = 0;
    if (view)
        *view = -1;
    const char* colon = strchr(spec, ':');
    const char* eq = strchr(spec, '=');
    if (colon && (!eq || colon < eq)) {
        char* end = NULL;
        long f = strtol(spec, &end, 10);
        if (end == colon) {
            *frame = (int)f;
            spec = colon + 1;
        }
    }
    const char* slash = view ? strchr(spec, '/') : NULL;
    if (slash && (!(eq = strchr(spec, '=')) || slash < eq)) {
        *view = atoi(spec);
        spec = slash + 1;
    }
    return spec;
}

// "a,b,c" one item at a time: the next item into `item`, the rest returned, NULL past the end.
static const char* _item(const char* list, char* item, size_t size) {
    if (!list || !*list)
        return NULL;
    const char* comma = strchr(list, ',');
    const size_t n = comma ? (size_t)(comma - list) : strlen(list);
    snprintf(item, size, "%.*s", (int)n, list);
    return comma ? comma + 1 : list + n;
}

static bool _state(const char* spec, SceneTagState* s) {
    memset(s, 0, sizeof(*s));
    if (strcmp(spec, "none") == 0)
        return true;
    char item[64];
    for (const char* rest = spec; (rest = _item(rest, item, sizeof(item)));) {
        if (strncmp(item, "colour=", 7) == 0)
            s->colour = (uint32_t)strtoul(item + 7, NULL, 16);
        else if (strncmp(item, "outline=", 8) == 0)
            s->outline = (uint32_t)strtoul(item + 8, NULL, 16);
        else if (strncmp(item, "dim=", 4) == 0)
            s->dim = (float)atof(item + 4);
        else if (strcmp(item, "hidden") == 0)
            s->hidden = true;
        else if (strcmp(item, "lit") == 0)
            s->lit = true;
        else
            return false;
    }
    return true;
}

static bool _class(const char* spec, SceneTagClass* k) {
    memset(k, 0, sizeof(*k));
    if (strcmp(spec, "none") == 0)
        return true;
    char item[64];
    for (const char* rest = spec; (rest = _item(rest, item, sizeof(item)));) {
        if (strncmp(item, "colour=", 7) == 0)
            k->colour = (uint32_t)strtoul(item + 7, NULL, 16);
        else if (strncmp(item, "see=", 4) == 0)
            k->see_through = (float)atof(item + 4);
        else if (strncmp(item, "flat=", 5) == 0)
            k->flat = (float)atof(item + 5);
        else if (strcmp(item, "hidden") == 0)
            k->hidden = true;
        else
            return false;
    }
    return true;
}

int render_tags_parse(RenderTags* t, int argc, char** argv, int* i) {
    const char* flag = argv[*i];
    const bool takes = strcmp(flag, "--node-tag") == 0 || strcmp(flag, "--piece-tags") == 0 ||
                       strcmp(flag, "--tag") == 0 || strcmp(flag, "--tag-class") == 0 ||
                       strcmp(flag, "--tag-fade") == 0 || strcmp(flag, "--tag-views") == 0;
    if (strcmp(flag, "--tag-table") == 0) {
        t->table = true;
        return 1;
    }
    if (!takes)
        return 0;
    if (*i + 1 >= argc) {
        fprintf(stderr, "Error: %s requires an argument\n", flag);
        return -1;
    }
    const char* spec = argv[++*i];
    int frame = 0, view = -1;
    if (strcmp(flag, "--tag-views") == 0) {
        t->table_views = (unsigned)strtoul(spec, NULL, 0);
        t->table_views_set = true;
        return 1;
    }
    if (strcmp(flag, "--tag-fade") == 0) {
        spec = _prefix(spec, &frame, &view);
        if (t->class_count >= RENDER_TAGS_MAX)
            goto full;
        t->classes[t->class_count].frame = frame;
        t->classes[t->class_count].view = view;
        t->classes[t->class_count].klass = -1;
        t->classes[t->class_count++].fade = (float)atof(spec);
        return 1;
    }
    const bool class_flag = strcmp(flag, "--tag-class") == 0;
    spec = _prefix(spec, &frame, class_flag ? &view : NULL);
    const char* eq = strchr(spec, '=');
    if (!eq || eq == spec) {
        fprintf(stderr, "Error: %s wants %s=...\n", flag,
                strcmp(flag, "--tag") == 0 ? "TAG"
                : class_flag               ? "CLASS"
                                           : "NAME");
        return -1;
    }
    if (strcmp(flag, "--node-tag") == 0) {
        if (t->node_count >= RENDER_TAGS_MAX)
            goto full;
        snprintf(t->nodes[t->node_count].name, sizeof(t->nodes[0].name), "%.*s", (int)(eq - spec),
                 spec);
        char* end = NULL;
        t->nodes[t->node_count].tag = (uint32_t)strtoul(eq + 1, &end, 0);
        t->nodes[t->node_count].klass = (uint8_t)(end && *end == ':' ? atoi(end + 1) : 0);
        t->nodes[t->node_count++].frame = frame;
        return 1;
    }
    if (strcmp(flag, "--piece-tags") == 0) {
        if (t->piece_count >= RENDER_TAGS_MAX)
            goto full;
        snprintf(t->pieces[t->piece_count].name, sizeof(t->pieces[0].name), "%.*s",
                 (int)(eq - spec), spec);
        t->pieces[t->piece_count++].first = (uint32_t)strtoul(eq + 1, NULL, 0);
        return 1;
    }
    if (strcmp(flag, "--tag") == 0) {
        if (t->row_count >= RENDER_TAGS_MAX)
            goto full;
        t->rows[t->row_count].frame = frame;
        t->rows[t->row_count].tag = (uint32_t)strtoul(spec, NULL, 0);
        if (!_state(eq + 1, &t->rows[t->row_count].state)) {
            fprintf(stderr, "Error: --tag: cannot read '%s'\n", eq + 1);
            return -1;
        }
        t->row_count++;
        t->table = true;
        return 1;
    }
    // --tag-class
    if (t->class_count >= RENDER_TAGS_MAX)
        goto full;
    t->classes[t->class_count].frame = frame;
    t->classes[t->class_count].view = view;
    t->classes[t->class_count].klass = atoi(spec);
    if (t->classes[t->class_count].klass < 1 ||
        t->classes[t->class_count].klass >= SCENE_TAG_CLASSES ||
        !_class(eq + 1, &t->classes[t->class_count].row)) {
        fprintf(stderr, "Error: --tag-class: cannot read '%s'\n", spec);
        return -1;
    }
    t->class_count++;
    return 1;
full:
    fprintf(stderr, "Error: %s given more than %d times\n", flag, RENDER_TAGS_MAX);
    return -1;
}

// The root of vertex v's piece, by union-find with path halving.
static uint32_t _find(uint32_t* parent, uint32_t v) {
    while (parent[v] != v)
        v = parent[v] = parent[parent[v]];
    return v;
}

static void _join(uint32_t* parent, uint32_t a, uint32_t b) {
    a = _find(parent, a), b = _find(parent, b);
    if (a != b)
        parent[a < b ? b : a] = a < b ? a : b;
}

// Vertices joined by a triangle or by standing at the same place are one piece: a box is six
// faces of their own corners, which only their places join.
static void _tag_pieces(Mesh* mesh, uint32_t first, const char* name) {
    const size_t n = mesh->vertex_count;
    uint32_t* parent = malloc(n * sizeof(uint32_t));
    uint32_t* order = calloc(n, sizeof(uint32_t)); // a root's piece number plus one
    uint32_t* tags = malloc(n * sizeof(uint32_t));
    if (!parent || !order || !tags) {
        free(parent), free(order), free(tags);
        return;
    }
    for (size_t v = 0; v < n; v++)
        parent[v] = (uint32_t)v;
    for (size_t k = 0; k + 2 < mesh->index_count; k += 3) {
        _join(parent, mesh->indices[k], mesh->indices[k + 1]);
        _join(parent, mesh->indices[k], mesh->indices[k + 2]);
    }
    // Coincident corners: a quadratic scan, which the small meshes this is for can afford.
    for (size_t a = 0; a < n; a++)
        for (size_t b = a + 1; b < n; b++)
            if (memcmp(&mesh->vertices[3 * a], &mesh->vertices[3 * b], 3 * sizeof(float)) == 0)
                _join(parent, (uint32_t)a, (uint32_t)b);
    uint32_t pieces = 0;
    for (size_t v = 0; v < n; v++) {
        const uint32_t root = _find(parent, (uint32_t)v);
        if (!order[root])
            order[root] = ++pieces;
        tags[v] = first + order[root] - 1;
    }
    free(mesh->tags);
    mesh->tags = tags;
    mesh_upload(mesh);
    free(parent);
    free(order);
    printf("tags: %s's mesh is %u pieces, tags %u to %u\n", name, pieces, first,
           first + pieces - 1);
}

static SceneTags* g_table = NULL;

void render_tags_frame(const RenderTags* t, Engine* engine, SceneView* driver, SceneView** grid,
                       int grid_count, int frame) {
    Scene* scene = engine_get_scene(engine);
    if (!scene)
        return;
    const int first = frame <= 1;
    for (int k = 0; k < t->node_count; k++) {
        if (!(t->nodes[k].frame == frame || (t->nodes[k].frame <= 1 && first)))
            continue;
        SceneNode* node = node_find(scene->root_node, t->nodes[k].name);
        if (!node) {
            fprintf(stderr, "Warning: --node-tag: no node '%s'\n", t->nodes[k].name);
            continue;
        }
        node->tag = t->nodes[k].tag;
        node->tag_class = t->nodes[k].klass;
    }
    for (int k = 0; first && k < t->piece_count; k++) {
        SceneNode* node = node_find(scene->root_node, t->pieces[k].name);
        if (!node || node->mesh_count == 0 || !node->meshes[0]) {
            fprintf(stderr, "Warning: --piece-tags: no node '%s' with a mesh\n", t->pieces[k].name);
            continue;
        }
        _tag_pieces(node->meshes[0], t->pieces[k].first, t->pieces[k].name);
    }
    if (t->table && !g_table)
        g_table = create_scene_tags();
    for (int k = 0; g_table && k < t->row_count; k++)
        if (t->rows[k].frame == frame || (t->rows[k].frame <= 1 && first))
            scene_tags_set(g_table, t->rows[k].tag, t->rows[k].state);

    // Each view's classes and fade, every frame from the one each was given at: the grid takes the
    // driver's settings afresh each frame, so a row written once would last a frame.
    SceneView* views[1 + 64];
    int count = 0;
    views[count++] = driver;
    for (int i = 0; i < grid_count && count < (int)(sizeof(views) / sizeof(views[0])); i++)
        views[count++] = grid[i];
    for (int v = 0; v < count; v++) {
        SceneView* view = views[v];
        if (!view)
            continue;
        const int index = v - 1; // the grid's number; -1 the driver
        for (int k = 0; k < t->class_count; k++) {
            if (t->classes[k].frame > frame)
                continue;
            if (t->classes[k].view >= 0 && t->classes[k].view != index)
                continue;
            if (t->classes[k].klass < 0)
                view->settings.tag_fade = t->classes[k].fade;
            else
                view->settings.tag_classes[t->classes[k].klass] = t->classes[k].row;
        }
        const bool borrows =
            index < 0 || !t->table_views_set || ((t->table_views >> index) & 1u) != 0;
        view->tags = borrows ? g_table : NULL;
    }
}
