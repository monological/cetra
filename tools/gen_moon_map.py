#!/usr/bin/env python3
"""Generate cetra/src/moon_map.h -- the Moon's maria, where the maria actually are.

WHY THIS IS DATA AND NOT NOISE. The lunar near side is a historical accident, not a
process: Mare Imbrium is a 3.9-billion-year-old impact basin that happened to be flooded
with basalt, and Tycho's rays point where they point because of one impact. No noise
function reproduces that, because there is no generating process to model -- which is why
a purely procedural moon reads as *a* cratered moon and never as *the* Moon, however well
its octaves are tuned.

WHY IT IS SMALL. Only the LOW-FREQUENCY layout has to come from data. Where the seas are
is what makes the face recognisable; the crater and grain detail on top of it is genuinely
statistical, stays procedural, and therefore stays sharp at any --moon-size where a
photograph would blur. So this carries 256x128 single-channel coverage and nothing else --
32 KB, against the multi-megabyte albedo mosaic a photographic answer would need.

WHY IT IS GENERATED RATHER THAN FETCHED. It is built from the published selenographic
centres and extents of the named maria, so it needs no download, no licence and no binary
asset in the tree -- and `fixture-gen` can assert it reproduces byte-identically, which a
downloaded image could not. The shapes are lobed rather than circular because real basins
are, but they are not claimed to be photographic: this is the right seas in the right
places at the right sizes, which is what the eye actually recognises at a few hundred
pixels across.

Longitudes are POSITIVE EAST, the IAU convention, matching the coordinates as published.

Regenerate: python3 tools/gen_moon_map.py
"""

import math
import os
import sys

W, H = 256, 128
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cetra", "src",
                   "moon_map.h")

# The named maria: (name, latitude N, longitude E, radius in degrees of arc, depth).
# Centres and extents from the standard gazetteer values; `depth` is how dark that sea
# runs relative to the highlands, which really does differ between them -- Crisium and
# Serenitatis are notably darker than Frigoris.
MARIA = [
    ("Oceanus Procellarum", 18.4, -57.4, 26.0, 1.00),
    ("Mare Imbrium", 32.8, -15.6, 18.0, 1.00),
    ("Mare Serenitatis", 28.0, 17.5, 12.0, 1.05),
    ("Mare Tranquillitatis", 8.5, 31.4, 14.0, 0.95),
    ("Mare Fecunditatis", -7.8, 51.3, 11.5, 0.90),
    ("Mare Nectaris", -15.2, 34.6, 8.0, 0.95),
    ("Mare Crisium", 17.0, 59.1, 9.0, 1.05),
    ("Mare Humorum", -24.4, -38.6, 8.0, 0.95),
    ("Mare Nubium", -21.3, -16.6, 11.0, 0.85),
    ("Mare Cognitum", -10.0, -23.1, 6.0, 0.85),
    ("Mare Vaporum", 13.3, 3.6, 6.5, 0.85),
    ("Mare Frigoris", 56.0, 1.4, 8.0, 0.70),
    ("Mare Insularum", 7.5, -30.9, 7.5, 0.80),
    ("Sinus Aestuum", 10.9, -8.8, 5.0, 0.80),
    ("Mare Australe", -38.9, 93.0, 12.0, 0.60),
    ("Mare Smythii", 1.3, 87.5, 7.0, 0.70),
    ("Mare Marginis", 13.3, 86.1, 6.0, 0.65),
    ("Mare Ingenii", -33.7, 163.5, 8.0, 0.55),
    ("Mare Moscoviense", 27.3, 147.9, 6.0, 0.60),
    ("Mare Orientale", -19.4, -92.8, 9.0, 0.75),
]

# Frigoris is a long arc rather than a disc, so it gets extra lobes along its length.
# Same for Procellarum, which is an ocean and the one feature a single blob cannot carry.
EXTRA_LOBES = [
    ("Mare Frigoris", [(58.0, -25.0, 6.5), (57.0, -45.0, 5.5), (54.0, 25.0, 6.0),
                       (50.0, 45.0, 5.0)]),
    ("Oceanus Procellarum", [(34.0, -45.0, 12.0), (5.0, -55.0, 14.0), (-5.0, -50.0, 11.0),
                             (25.0, -65.0, 12.0), (-15.0, -45.0, 9.0)]),
    ("Mare Imbrium", [(38.0, -25.0, 12.0), (25.0, -5.0, 10.0)]),
    ("Mare Tranquillitatis", [(2.0, 22.0, 9.0), (14.0, 40.0, 8.0)]),
]


def _hash(i, j, k):
    """A small deterministic hash, so the boundary wobble is reproducible anywhere."""
    h = (i * 374761393 + j * 668265263 + k * 2147483647) & 0xFFFFFFFF
    h = (h ^ (h >> 13)) * 1274126177 & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0


def _lobe_noise(bearing, seed):
    """Wobble on a basin's radius, as a function of BEARING around its centre.

    Bearing, not raw lat/lon offset -- that was the first version and it is why the seas
    came out as overlapping circles. Across a ten-degree basin a harmonic in longitude
    completes barely half a cycle, so the radius was very nearly constant and every sea
    was a disc. In bearing the same harmonics go round the basin several times, which is
    what puts inlets and headlands on the shore.

    Three harmonics with hashed phases: real basins are lobed from the fracture pattern
    under them, and one harmonic gives an egg rather than a coastline.
    """
    p1 = _hash(seed, 1, 0) * math.tau
    p2 = _hash(seed, 2, 0) * math.tau
    p3 = _hash(seed, 3, 0) * math.tau
    return (0.22 * math.sin(2.0 * bearing + p1) + 0.15 * math.sin(3.0 * bearing + p2) +
            0.09 * math.sin(5.0 * bearing + p3))


def _angular_distance(lat0, lon0, lat1, lon1):
    """Great-circle separation in degrees -- the sphere's own metric.

    Not a flat lat/lon distance: that stretches every basin toward the poles, which puts
    Frigoris (56 N) across half the disc.
    """
    p0, p1 = math.radians(lat0), math.radians(lat1)
    d = math.radians(lon1 - lon0)
    c = math.sin(p0) * math.sin(p1) + math.cos(p0) * math.cos(p1) * math.cos(d)
    return math.degrees(math.acos(max(-1.0, min(1.0, c))))


def build():
    """Coverage in [0,1] per texel: 0 highland, 1 the darkest sea."""
    lobes = []
    for idx, (name, lat, lon, radius, depth) in enumerate(MARIA):
        lobes.append((lat, lon, radius, depth, idx))
        for ename, extra in EXTRA_LOBES:
            if ename != name:
                continue
            for elat, elon, erad in extra:
                lobes.append((elat, elon, erad, depth, idx))

    data = bytearray(W * H)
    for y in range(H):
        # Texel CENTRES, not edges: at 128 rows the half-texel is 0.7 degrees, which is
        # enough to walk a basin off its published latitude.
        lat = 90.0 - (y + 0.5) * (180.0 / H)
        for x in range(W):
            lon = -180.0 + (x + 0.5) * (360.0 / W)
            best = 0.0
            for (clat, clon, radius, depth, idx) in lobes:
                d = _angular_distance(lat, lon, clat, clon)
                # Bearing from the basin centre to this texel. cos(lat) on the
                # longitude leg, or every basin's wobble winds up as the pole is
                # approached and Frigoris at 56 N gets a different shape from
                # Nubium at 21 S for no reason but its latitude.
                bearing = math.atan2((lon - clon) * math.cos(math.radians(clat)),
                                     lat - clat)
                r = radius * (1.0 + _lobe_noise(bearing, idx))
                if d >= r:
                    continue
                # Flat across the basin floor, falling off over the outer fifth. A sea
                # has a shore, not a gradient.
                t = d / r
                v = depth * (1.0 - _smoothstep(0.78, 1.0, t))
                best = max(best, v)
            data[y * W + x] = int(round(max(0.0, min(1.0, best)) * 255.0))
    return data


def _smoothstep(a, b, x):
    t = max(0.0, min(1.0, (x - a) / (b - a)))
    return t * t * (3.0 - 2.0 * t)


def emit(data):
    rows = []
    for i in range(0, len(data), 16):
        rows.append("    " + " ".join(f"{b}," for b in data[i:i + 16]))
    body = "\n".join(rows)
    names = "\n".join(f" *   {n}" for n, *_ in MARIA)
    return f"""/* GENERATED by tools/gen_moon_map.py. DO NOT EDIT.
 * Regenerate: python3 tools/gen_moon_map.py
 *
 * The Moon's maria as {W}x{H} single-channel coverage, equirectangular,
 * row 0 at +90 latitude, column 0 at -180 longitude, POSITIVE EAST (IAU).
 * 0 = highland, 255 = the darkest sea floor.
 *
 * Built from the published selenographic centres and extents of the named
 * maria, because where the seas are is a historical accident no noise
 * function can produce -- see the generator's header. Only this
 * low-frequency layout is data; the crater and grain detail on top of it
 * stays procedural in moon.glsl, which is what keeps the surface sharp at
 * any --moon-size where a photograph would blur.
 *
 * Carried:
{names}
 */

#ifndef _MOON_MAP_H_
#define _MOON_MAP_H_

#define MOON_MAP_W {W}
#define MOON_MAP_H {H}

static const unsigned char MOON_MAP[{W} * {H}] = {{
{body}
}};

#endif // _MOON_MAP_H_
"""


if __name__ == "__main__":
    out = os.path.normpath(OUT)
    text = emit(build())
    if "--check" in sys.argv:
        with open(out) as fh:
            sys.exit(0 if fh.read() == text else 1)
    with open(out, "w") as fh:
        fh.write(text)
    print(f"wrote {out} ({W}x{H}, {W * H} bytes of coverage)")
