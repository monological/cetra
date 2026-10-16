#include <math.h>

#include "kit.h"
#include "pull_cord.h"

#define SWEEP      1.1f  // seconds end to end at first
#define QUICKEN    0.94f // the sweep's share of itself after each good pull
#define ZONE       0.16f // the lit zone's share of the bar
#define ZONE_EDGE  0.06f // and how near either end it may lie
#define ZONE_APART 0.25f // a new zone at least this far along from the last
#define FLASH      0.3f  // seconds a pull's flash lasts
#define FADE       0.15f // seconds the bar takes to come up and to go

// The bar, in window points: its size, how far down the window its middle is, and the plate's
// margin round it, with room over it for the pulls' marks.
#define BAR_W     380.0f
#define BAR_H     12.0f
#define BAR_DOWN  0.7f
#define PLATE_PAD 18.0f
#define MARKS_UP  26.0f
#define MARK_R    6.0f
#define MARKER_W  5.0f
#define MARKER_H  28.0f

static const vec4 PLATE = {0.0f, 0.0f, 0.0f, 0.5f};
static const vec4 TRACK = {0.88f, 0.88f, 0.84f, 0.2f};
static const vec4 AMBER = {0.96f, 0.69f, 0.22f, 0.9f};
static const vec4 MARKER = {0.95f, 0.95f, 0.92f, 1.0f};
static const vec4 MISS = {0.82f, 0.26f, 0.2f, 1.0f};
static const vec4 MARK_EMPTY = {0.88f, 0.88f, 0.84f, 0.35f};

// Where the marker is along the bar, 0..1: up it and back down, at the sweep's pace.
static float marker(const PullCord* pc) {
    const float t = fmodf(pc->run / pc->sweep, 2.0f);
    return t <= 1.0f ? t : 2.0f - t;
}

// A new zone somewhere else along the bar.
static void move_zone(PullCord* pc) {
    const float span = 1.0f - 2.0f * ZONE_EDGE - ZONE;
    for (int i = 0; i < 8; i++) {
        const float z = ZONE_EDGE + span * kit_xrnd(&pc->state);
        if (fabsf(z - pc->zone) >= ZONE_APART) {
            pc->zone = z;
            return;
        }
    }
    pc->zone = pc->zone < 0.5f ? 1.0f - ZONE_EDGE - ZONE : ZONE_EDGE;
}

void pull_cord_open(PullCord* pc, unsigned int seed) {
    const float alpha = pc->alpha;
    *pc = (PullCord){.sweep = SWEEP, .zone = 0.5f, .since = FLASH, .alpha = alpha};
    pc->state = seed ? seed : 0x9e3779b9u;
    move_zone(pc);
}

void pull_cord_update(PullCord* pc, bool shown, float dt) {
    pc->alpha = fminf(fmaxf(pc->alpha + (shown ? dt : -dt) / FADE, 0.0f), 1.0f);
    if (!shown)
        return;
    pc->run += dt;
    pc->since += dt;
}

bool pull_cord_pull(PullCord* pc) {
    // Good while the marker as drawn touches the zone, not only its middle.
    const float m = marker(pc), grace = 0.5f * MARKER_W / BAR_W;
    pc->last_good = m >= pc->zone - grace && m <= pc->zone + ZONE + grace;
    pc->since = 0.0f;
    if (pc->last_good) {
        pc->good++;
        // Quicker from where it is: the run scaled with the sweep, or the marker would jump to
        // wherever the new pace puts it, as often as not going the other way.
        pc->sweep *= QUICKEN;
        pc->run *= QUICKEN;
        move_zone(pc);
    }
    return pc->last_good;
}

bool pull_cord_caught(const PullCord* pc) {
    return pc->good >= PULL_CORD_PULLS;
}

static void faded(const float* c, float alpha, vec4 out) {
    glm_vec4_copy((float*)c, out);
    out[3] *= alpha;
}

void pull_cord_draw(UIElement* el, UIDrawList* dl, void* user) {
    const PullCord* pc = user;
    if (pc->alpha <= 0.0f)
        return;
    const UIRect w = el->rect;
    const float x = w.x + 0.5f * (w.w - BAR_W), y = w.y + BAR_DOWN * w.h - 0.5f * BAR_H;
    vec4 c = {0.0f, 0.0f, 0.0f, 0.0f};
    faded(PLATE, pc->alpha, c);
    ui_draw_rounded(dl,
                    (UIRect){x - PLATE_PAD, y - PLATE_PAD - MARKS_UP, BAR_W + 2.0f * PLATE_PAD,
                             BAR_H + 2.0f * PLATE_PAD + MARKS_UP},
                    8.0f, c, NULL, 0.0f);
    // The pulls had, as marks over the bar's middle.
    for (int i = 0; i < PULL_CORD_PULLS; i++) {
        const float cx =
            x + 0.5f * BAR_W + ((float)i - 0.5f * (PULL_CORD_PULLS - 1)) * 4.0f * MARK_R;
        const UIRect mark = {cx - MARK_R, y - MARKS_UP + 0.5f * MARK_R, 2.0f * MARK_R,
                             2.0f * MARK_R};
        faded(i < pc->good ? AMBER : MARK_EMPTY, pc->alpha, c);
        ui_draw_rounded(dl, mark, MARK_R, c, NULL, 0.0f);
    }
    faded(TRACK, pc->alpha, c);
    ui_draw_rounded(dl, (UIRect){x, y, BAR_W, BAR_H}, 0.5f * BAR_H, c, NULL, 0.0f);
    // The zone, brighter for a moment after a good pull.
    const float flash = pc->since < FLASH ? 1.0f - pc->since / FLASH : 0.0f;
    faded(AMBER, pc->alpha * (0.75f + 0.25f * (pc->last_good ? flash : 0.0f)), c);
    ui_draw_rounded(dl, (UIRect){x + pc->zone * BAR_W, y, ZONE * BAR_W, BAR_H}, 0.5f * BAR_H, c,
                    NULL, 0.0f);
    // The marker, red for a moment after a weak pull.
    vec4 tint = {0.0f, 0.0f, 0.0f, 0.0f};
    glm_vec4_lerp((float*)MARKER, (float*)MISS, pc->last_good ? 0.0f : flash, tint);
    faded(tint, pc->alpha, c);
    const float mx = x + marker(pc) * BAR_W;
    ui_draw_rounded(
        dl, (UIRect){mx - 0.5f * MARKER_W, y + 0.5f * BAR_H - 0.5f * MARKER_H, MARKER_W, MARKER_H},
        0.5f * MARKER_W, c, NULL, 0.0f);
}
