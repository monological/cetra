// The SSR marker: what a surface tells screen-space reflection, in the alpha of the normals
// G-buffer. A RANGE rather than a sign, so every reader asks here instead of testing the value:
//
//   0        unmarked: no trace, the environment's reflection stands
//   (-1, 0)  the shadow catcher, written -falloff: its magnitude is the edge fade
//   < -1     wet ground (spec 13.9), written -(1 + film), whose roughness rides the aux buffer
//
// A sign test anywhere hands wet ground the catcher's treatment.

float ssrMarkWet(float film)
{
    return -(1.0 + film);
}

bool ssrMarkerIsWet(float a)
{
    return a < -1.0;
}

bool ssrMarkerIsCatcher(float a)
{
    return a < 0.0 && a >= -1.0;
}

// How strongly a marked surface reflects, 0..1: the catcher's edge fade, wet ground's film.
float ssrMarkerFade(float a)
{
    return ssrMarkerIsWet(a) ? clamp(-a - 1.0, 0.0, 1.0) : clamp(-a, 0.0, 1.0);
}

// 2 wet ground, 1 the catcher, 0 unmarked.
int ssrMarkerClass(float a)
{
    return a < -1.0 ? 2 : (a < 0.0 ? 1 : 0);
}
