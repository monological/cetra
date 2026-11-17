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

// The flag's argument, consumed; NULL, said, when there is none.
static const char* _arg(int argc, char** argv, int* i) {
    if (*i + 1 >= argc) {
        fprintf(stderr, "Error: %s requires an argument\n", argv[*i]);
        return NULL;
    }
    return argv[++*i];
}

// The '=' in "WHAT=...", or NULL, said, when there is none or nothing before it.
static const char* _named(const char* flag, const char* spec, const char* what) {
    const char* eq = strchr(spec, '=');
    if (!eq || eq == spec) {
        fprintf(stderr, "Error: %s wants %s=...\n", flag, what);
        return NULL;
    }
    return eq;
}

// Whether a list already holds `count` entries, said when it does.
static bool _full(const char* flag, int count) {
    if (count < RENDER_TAGS_MAX)
        return false;
    fprintf(stderr, "Error: %s given more than %d times\n", flag, RENDER_TAGS_MAX);
    return true;
}

// The entry just added at `k` moved back past those due later, so a list stays in the order its
// entries fall due, those due together in the order given.
static void _settle_class(RenderTagClass* list, int k) {
    for (; k > 0 && list[k - 1].frame > list[k].frame; k--) {
        const RenderTagClass later = list[k - 1];
        list[k - 1] = list[k];
        list[k] = later;
    }
}

static void _settle_fade(RenderTagFade* list, int k) {
    for (; k > 0 && list[k - 1].frame > list[k].frame; k--) {
        const RenderTagFade later = list[k - 1];
        list[k - 1] = list[k];
        list[k] = later;
    }
}

int render_tags_parse(RenderTags* t, int argc, char** argv, int* i) {
    const char* flag = argv[*i];
    int frame = 0, view = -1;
    if (strcmp(flag, "--tag-table") == 0) {
        t->table = true;
        return 1;
    }
    if (strcmp(flag, "--tag-views") == 0) {
        const char* spec = _arg(argc, argv, i);
        if (!spec)
            return -1;
        t->table_views = (unsigned)strtoul(spec, NULL, 0);
        t->table_views_set = true;
        return 1;
    }
    if (strcmp(flag, "--tag-fade") == 0) {
        const char* spec = _arg(argc, argv, i);
        if (!spec || _full(flag, t->fade_count))
            return -1;
        spec = _prefix(spec, &frame, &view);
        t->fades[t->fade_count].frame = frame;
        t->fades[t->fade_count].view = view;
        t->fades[t->fade_count].fade = (float)atof(spec);
        _settle_fade(t->fades, t->fade_count++);
        return 1;
    }
    if (strcmp(flag, "--node-tag") == 0) {
        const char* spec = _arg(argc, argv, i);
        if (!spec || _full(flag, t->node_count))
            return -1;
        spec = _prefix(spec, &frame, NULL);
        const char* eq = _named(flag, spec, "NAME");
        if (!eq)
            return -1;
        snprintf(t->nodes[t->node_count].name, sizeof(t->nodes[0].name), "%.*s", (int)(eq - spec),
                 spec);
        char* end = NULL;
        t->nodes[t->node_count].tag = (uint32_t)strtoul(eq + 1, &end, 0);
        t->nodes[t->node_count].klass = (uint8_t)(end && *end == ':' ? atoi(end + 1) : 0);
        t->nodes[t->node_count++].frame = frame;
        return 1;
    }
    if (strcmp(flag, "--piece-tags") == 0) {
        const char* spec = _arg(argc, argv, i);
        if (!spec || _full(flag, t->piece_count))
            return -1;
        const char* eq = _named(flag, spec, "NAME");
        if (!eq)
            return -1;
        snprintf(t->pieces[t->piece_count].name, sizeof(t->pieces[0].name), "%.*s",
                 (int)(eq - spec), spec);
        t->pieces[t->piece_count++].first = (uint32_t)strtoul(eq + 1, NULL, 0);
        return 1;
    }
    if (strcmp(flag, "--tag") == 0) {
        const char* spec = _arg(argc, argv, i);
        if (!spec || _full(flag, t->row_count))
            return -1;
        spec = _prefix(spec, &frame, NULL);
        const char* eq = _named(flag, spec, "TAG");
        if (!eq)
            return -1;
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
    if (strcmp(flag, "--tag-class") == 0) {
        const char* spec = _arg(argc, argv, i);
        if (!spec || _full(flag, t->class_count))
            return -1;
        spec = _prefix(spec, &frame, &view);
        const char* eq = _named(flag, spec, "CLASS");
        if (!eq)
            return -1;
        t->classes[t->class_count].frame = frame;
        t->classes[t->class_count].view = view;
        t->classes[t->class_count].klass = atoi(spec);
        if (t->classes[t->class_count].klass < 1 ||
            t->classes[t->class_count].klass >= SCENE_TAG_CLASSES ||
            !_class(eq + 1, &t->classes[t->class_count].row)) {
            fprintf(stderr, "Error: --tag-class: cannot read '%s'\n", spec);
            return -1;
        }
        _settle_class(t->classes, t->class_count++);
        return 1;
    }
    return 0;
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

// A vertex's place, compared by its bytes, as two corners at one place are written alike.
typedef struct Corner {
    float at[3];
    uint32_t vertex;
} Corner;

static int _corner_order(const void* a, const void* b) {
    return memcmp(((const Corner*)a)->at, ((const Corner*)b)->at, sizeof(((Corner*)0)->at));
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
    // Coincident corners, found as neighbours once the corners are sorted by their places' bytes.
    Corner* corners = malloc(n * sizeof(Corner));
    if (corners) {
        for (size_t v = 0; v < n; v++) {
            memcpy(corners[v].at, &mesh->vertices[3 * v], sizeof(corners[v].at));
            corners[v].vertex = (uint32_t)v;
        }
        qsort(corners, n, sizeof(Corner), _corner_order);
        for (size_t v = 1; v < n; v++)
            if (_corner_order(&corners[v - 1], &corners[v]) == 0)
                _join(parent, corners[v - 1].vertex, corners[v].vertex);
        free(corners);
    }
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

// Whether `name` matches `pattern`, where `*` stands for any run of characters.
static bool _matches(const char* pattern, const char* name) {
    if (*pattern == '\0')
        return *name == '\0';
    if (*pattern == '*')
        return _matches(pattern + 1, name) || (*name && _matches(pattern, name + 1));
    return *pattern == *name && _matches(pattern + 1, name + 1);
}

// Every node under `node` whose name matches `pattern` given the tag and class; how many.
static int _tag_nodes(SceneNode* node, const char* pattern, uint32_t tag, uint8_t klass) {
    if (!node)
        return 0;
    int tagged = 0;
    if (node->name && _matches(pattern, node->name)) {
        node->tag = tag;
        node->tag_class = klass;
        tagged++;
    }
    for (size_t i = 0; i < node->children_count; i++)
        tagged += _tag_nodes(node->children[i], pattern, tag, klass);
    return tagged;
}

static SceneTags* g_table = NULL;

// Whether a write given at frame `at` is made this frame: at its frame, or at the first for one
// given at 1 or none.
static bool _due(int at, int frame) {
    return at == frame || (at <= 1 && frame <= 1);
}

void render_tags_frame(const RenderTags* t, Engine* engine, SceneView* driver, SceneView** grid,
                       int grid_count, int frame) {
    Scene* scene = engine_get_scene(engine);
    if (!scene)
        return;
    const bool first = frame <= 1;
    for (int k = 0; k < t->node_count; k++) {
        if (!_due(t->nodes[k].frame, frame))
            continue;
        const int tagged =
            _tag_nodes(scene->root_node, t->nodes[k].name, t->nodes[k].tag, t->nodes[k].klass);
        if (!tagged)
            fprintf(stderr, "Warning: --node-tag: no node '%s'\n", t->nodes[k].name);
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
        if (_due(t->rows[k].frame, frame))
            scene_tags_set(g_table, t->rows[k].tag, t->rows[k].state);

    // Each view's classes and fade, every frame from the one each was given at, in the order they
    // fell due: the grid takes the driver's settings afresh each frame, so a row written once
    // would last a frame. `index` is the grid's number, -1 the driver.
    for (int index = -1; index < grid_count; index++) {
        SceneView* view = index < 0 ? driver : grid[index];
        if (!view)
            continue;
        for (int k = 0; k < t->class_count && t->classes[k].frame <= frame; k++)
            if (t->classes[k].view < 0 || t->classes[k].view == index)
                view->settings.tag_classes[t->classes[k].klass] = t->classes[k].row;
        for (int k = 0; k < t->fade_count && t->fades[k].frame <= frame; k++)
            if (t->fades[k].view < 0 || t->fades[k].view == index)
                view->settings.tag_fade = t->fades[k].fade;
        const bool borrows =
            index < 0 || !t->table_views_set || ((t->table_views >> index) & 1u) != 0;
        view->tags = borrows ? g_table : NULL;
    }
}
