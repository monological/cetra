#include <assert.h>
#include <math.h>

#include "road.h"

#define ROAD_MAX_POINTS 16
#define ROAD_DENSE      64    // points a span is drawn through before it is resampled by length
#define ROAD_UNDER      0.03f // the ground's depth under the surface, so a ribbon is never under it

static float catmull(float p0, float p1, float p2, float p3, float t) {
    const float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

// The line at even steps of its length, through the points by Catmull-Rom.
static void sample(Road* r) {
    const float (*p)[2] = r->desc->points;
    const int count = r->desc->point_count;
    assert(count >= 2 && count <= ROAD_MAX_POINTS);
    // Densely first, then resampled by length, so the steps are even whatever the spans are.
    static float px[(ROAD_MAX_POINTS - 1) * ROAD_DENSE + 1],
        pz[(ROAD_MAX_POINTS - 1) * ROAD_DENSE + 1], ps[(ROAD_MAX_POINTS - 1) * ROAD_DENSE + 1];
    int n = 0;
    for (int i = 0; i + 1 < count; i++) {
        const int i0 = i > 0 ? i - 1 : 0, i3 = i + 2 < count ? i + 2 : i + 1;
        for (int k = 0; k < ROAD_DENSE; k++) {
            const float t = (float)k / ROAD_DENSE;
            px[n] = catmull(p[i0][0], p[i][0], p[i + 1][0], p[i3][0], t);
            pz[n] = catmull(p[i0][1], p[i][1], p[i + 1][1], p[i3][1], t);
            n++;
        }
    }
    px[n] = p[count - 1][0];
    pz[n] = p[count - 1][1];
    n++;
    ps[0] = 0.0f;
    for (int i = 1; i < n; i++)
        ps[i] = ps[i - 1] + hypotf(px[i] - px[i - 1], pz[i] - pz[i - 1]);

    r->length = ps[n - 1];
    r->count = 0;
    int j = 0;
    for (float s = 0.0f; r->count < ROAD_SAMPLES; s += ROAD_STEP) {
        if (s > r->length)
            s = r->length;
        while (j + 1 < n - 1 && ps[j + 1] < s)
            j++;
        const float span = ps[j + 1] - ps[j];
        const float t = span > 0.0f ? (s - ps[j]) / span : 0.0f;
        r->x[r->count] = px[j] + (px[j + 1] - px[j]) * t;
        r->z[r->count] = pz[j] + (pz[j + 1] - pz[j]) * t;
        r->s[r->count] = s;
        r->count++;
        if (s >= r->length)
            break;
    }
    r->box[0] = r->box[2] = r->x[0];
    r->box[1] = r->box[3] = r->z[0];
    for (int i = 1; i < r->count; i++) {
        r->box[0] = fminf(r->box[0], r->x[i]);
        r->box[1] = fminf(r->box[1], r->z[i]);
        r->box[2] = fmaxf(r->box[2], r->x[i]);
        r->box[3] = fmaxf(r->box[3], r->z[i]);
    }
}

static Road* sampled(Road* r) {
    if (r->count == 0)
        sample(r);
    return r;
}

/*
 * The share of the way from y0 to y1 at `t` of the run between the flat ends, for one grade eased
 * in and out over `f` of it: a parabola into the grade, a line, a parabola out, meeting with the
 * same slope. A smoothstep's steepest point is half as steep again as its average, so a road
 * falling a given height under a given grade needs a third more length as a smoothstep.
 */
static float graded(float t, float f) {
    t = glm_clamp(t, 0.0f, 1.0f);
    const float k = 2.0f * f * (1.0f - f);
    if (t < f)
        return t * t / k;
    if (t > 1.0f - f)
        return 1.0f - (1.0f - t) * (1.0f - t) / k;
    return (t - 0.5f * f) / (1.0f - f);
}

float road_height(Road* road, float s) {
    const RoadDesc* d = sampled(road)->desc;
    if (d->rise == ROAD_RISE_GRADE) {
        const float run = road->length - d->flat_run - d->flat_end;
        const float f = run > 0.0f ? glm_clamp(d->ease / run, 1e-3f, 0.5f) : 0.5f;
        return d->y0 + (d->y1 - d->y0) * graded(run > 0.0f ? (s - d->flat_run) / run : 1.0f, f);
    }
    const float t = glm_smoothstep(d->flat_run, road->length - d->flat_end, s);
    return d->y0 + (d->y1 - d->y0) * t;
}

float road_distance(Road* road, float x, float z, float* along) {
    const Road* r = sampled(road);
    float best = 1e30f;
    *along = 0.0f;
    for (int i = 0; i + 1 < r->count; i++) {
        const float ax = r->x[i], az = r->z[i];
        const float bx = r->x[i + 1] - ax, bz = r->z[i + 1] - az;
        const float len2 = bx * bx + bz * bz;
        float t = len2 > 0.0f ? ((x - ax) * bx + (z - az) * bz) / len2 : 0.0f;
        t = glm_clamp(t, 0.0f, 1.0f);
        const float dx = x - (ax + bx * t), dz = z - (az + bz * t);
        const float d2 = dx * dx + dz * dz;
        if (d2 < best) {
            best = d2;
            *along = r->s[i] + (r->s[i + 1] - r->s[i]) * t;
        }
    }
    return sqrtf(best);
}

float road_carve(Road* road, float x, float z, float h) {
    const Road* r = sampled(road);
    const RoadDesc* d = r->desc;
    // Past the shoulder of every sample the carve's weight is exactly 0.
    const float reach = d->half + d->shoulder;
    if (x < r->box[0] - reach || x > r->box[2] + reach || z < r->box[1] - reach ||
        z > r->box[3] + reach)
        return h;
    float along = 0.0f;
    const float dist = road_distance(road, x, z, &along);
    const float w = 1.0f - glm_smoothstep(d->half, d->half + d->shoulder, dist);
    return h + (road_height(road, along) - ROAD_UNDER - h) * w;
}

void road_point(Road* road, float t, float* x, float* z) {
    const Road* r = sampled(road);
    const int i = (int)(glm_clamp(t, 0.0f, 1.0f) * (float)(r->count - 1));
    *x = r->x[i];
    *z = r->z[i];
}

void road_frame(Road* road, float t, float* x, float* z, float* dir_x, float* dir_z) {
    const Road* r = sampled(road);
    const int i = (int)(glm_clamp(t, 0.0f, 1.0f) * (float)(r->count - 1));
    const int a = i > 0 ? i - 1 : 0, b = i + 1 < r->count ? i + 1 : i;
    *x = r->x[i];
    *z = r->z[i];
    const float dx = r->x[b] - r->x[a], dz = r->z[b] - r->z[a];
    const float len = hypotf(dx, dz);
    *dir_x = len > 0.0f ? dx / len : 1.0f;
    *dir_z = len > 0.0f ? dz / len : 0.0f;
}

float road_length(Road* road) {
    return sampled(road)->length;
}

// The sample segment `s` falls in, and how far along it.
static int segment(const Road* r, float s, float* t) {
    int i = 0;
    while (i + 2 < r->count && r->s[i + 1] <= s)
        i++;
    const float span = r->s[i + 1] - r->s[i];
    *t = span > 0.0f ? glm_clamp((s - r->s[i]) / span, 0.0f, 1.0f) : 0.0f;
    return i;
}

void road_at(Road* road, float s, float* x, float* z) {
    const Road* r = sampled(road);
    float t = 0.0f;
    const int i = segment(r, s, &t);
    *x = r->x[i] + (r->x[i + 1] - r->x[i]) * t;
    *z = r->z[i] + (r->z[i + 1] - r->z[i]) * t;
}

/*
 * One station of the ribbon `s` metres along: its centre and the unit way the road runs. On a
 * sample, the sample and the run between its neighbours; between samples, a point on the segment
 * and that segment's run, so a ribbon can start and stop exactly where it meets something else.
 */
static void station(const Road* r, float s, float* x, float* z, float* tx, float* tz) {
    for (int i = 0; i < r->count; i++)
        if (r->s[i] == s) {
            const int a = i > 0 ? i - 1 : 0, b = i + 1 < r->count ? i + 1 : i;
            *x = r->x[i];
            *z = r->z[i];
            *tx = r->x[b] - r->x[a];
            *tz = r->z[b] - r->z[a];
            const float len = hypotf(*tx, *tz);
            *tx /= len;
            *tz /= len;
            return;
        }
    float t = 0.0f;
    const int i = segment(r, s, &t);
    *tx = r->x[i + 1] - r->x[i];
    *tz = r->z[i + 1] - r->z[i];
    *x = r->x[i] + *tx * t;
    *z = r->z[i] + *tz * t;
    const float len = hypotf(*tx, *tz);
    *tx /= len;
    *tz /= len;
}

void road_ribbon(Kit* kit, Road* road, int mat, float s0, float s1) {
    const Road* r = sampled(road);
    const float half = r->desc->half;
    const vec3 up = {0.0f, 1.0f, 0.0f};
    s0 = fmaxf(s0, 0.0f);
    s1 = fminf(s1, r->length);
    vec3 prev_l = {0}, prev_r = {0};
    // The stations: s0, every sample strictly between, s1.
    for (int i = -1; i <= r->count; i++) {
        float s;
        if (i < 0)
            s = s0;
        else if (i == r->count)
            s = s1;
        else if (r->s[i] > s0 && r->s[i] < s1)
            s = r->s[i];
        else
            continue;
        float x, z, tx, tz;
        station(r, s, &x, &z, &tx, &tz);
        const float y = road_height(road, s);
        const vec3 l = {x - tz * half, y, z + tx * half};
        const vec3 rt = {x + tz * half, y, z - tx * half};
        if (i >= 0)
            kit_quad_facing(kit, mat, prev_l, prev_r, rt, l, up);
        glm_vec3_copy((float*)l, prev_l);
        glm_vec3_copy((float*)rt, prev_r);
    }
}
